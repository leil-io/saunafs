package main

import (
	"bytes"
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"reflect"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/docker/docker/api/types"
	"github.com/docker/docker/api/types/container"
	"github.com/docker/docker/api/types/mount"
	"github.com/docker/docker/client"
	"github.com/docker/docker/errdefs"
	"github.com/docker/docker/pkg/stdcopy"
	"github.com/docker/go-units"
)

func panicIfErr(e error) {
	if e != nil {
		fmt.Fprintf(os.Stderr, "error type: %s\n", reflect.TypeOf(e))
		panic(e)
	}
}

type testOptions struct {
	Workers          int
	Suite            string
	Source           string
	TestPattern      string
	MountPoint       string
    SkipUpgradeTests bool
	AllOutput        bool
	DeleteContainers bool
	Multiplier       int
	CI               bool
}

var options testOptions

var originalCorePattern string = ""

func init() {
	flag.StringVar(&options.Suite, "suite", "SanityChecks", "Test suite to run")
	flag.StringVar(&options.Suite, "s", "SanityChecks", "shorthand for -suite")

	flag.StringVar(&options.TestPattern, "test", "*", "Test pattern to run")
	flag.StringVar(&options.TestPattern, "t", "*", "shorthand for -test")

	flag.IntVar(&options.Workers, "w", runtime.NumCPU(), "shorthand for -workers")
	flag.IntVar(&options.Workers, "workers", runtime.NumCPU(), "Number of containers to run tests")

	flag.IntVar(&options.Multiplier, "multiplier", 1, "Test timeout multiplier")

	flag.BoolVar(&options.AllOutput, "a", false, "shorthand for -all")
	flag.BoolVar(&options.AllOutput, "all", false, "Output all test results, including succeeding")

	flag.BoolVar(&options.AllOutput, "ci", false, "Run in CI mode (disable privileged modes and enhance security")

	flag.BoolVar(&options.DeleteContainers, "d", false, "shorthand for -d")
	flag.BoolVar(&options.DeleteContainers, "delete", false, "Delete all containers regardless if they failed or not")

	flag.BoolVar(&options.SkipUpgradeTests, "skip-upgrade", true, "Skip all upgrade tests from all suites")

	flag.StringVar(&options.MountPoint, "mount", "", "SaunaFS git repository to mount")
	flag.StringVar(&options.MountPoint, "m", "", "shorthand for -mount")
}

const (
	sourceDir   = "/opt/saunafs" // Set your source directory path here
	imageName   = "saunafs-test"
	corePattern = "/tmp/temp-cores/core-%e-%p-%t"
)

var hostConfig = container.HostConfig{
	Resources: container.Resources{
		Devices: []container.DeviceMapping{
			{
				PathOnHost:        "/dev/fuse",
				PathInContainer:   "/dev/fuse",
				CgroupPermissions: "rwm",
			},
		},
		Ulimits: []*units.Ulimit{
			{
				Name: "core",
				Soft: -1,
				Hard: -1,
			},
		},
	},
	Tmpfs: map[string]string{
		"/mnt/ramdisk": "rw,mode=1777,size=2g",
	},
	Privileged: true,
	CapAdd:     []string{"SYS_ADMIN", "SYS_PTRACE", "NET_ADMIN"},
}

func main() {
	flag.Parse()
	// Signal handling, so we cleanup properly
	ctx, cancel := context.WithCancel(context.Background())
	interrupt := make(chan os.Signal, 1)
	signal.Notify(interrupt, os.Interrupt)
	go func() {
		<-interrupt
		log.Printf("\nStopping tests, please wait...\n")
		log.SetOutput(io.Discard)
		cancel()
	}()

	if options.CI {
		hostConfig.Privileged = false
	}

	client, err := client.NewClientWithOpts(client.FromEnv, client.WithAPIVersionNegotiation())
	panicIfErr(err)
	defer client.Close()

	os.Exit(runTests(ctx, client))
}

type Test struct {
	Name          string
	TestSuite     string
	Ctx           context.Context
	Client        *client.Client
	ContainerName string
	Success       bool
	TestOutput    []byte
}

func runTests(ctx context.Context, client *client.Client) int {
	// This ctx will never be cancelled, good for cleanup
	alwaysCtx := context.Background()
	testNames := getTestGlobs(ctx, client)
	suite, ok := testNames[options.Suite]
	if !ok {
		fmt.Fprintf(os.Stderr, "Test suite %s not found, these suites are available:\n", options.Suite)
		for key := range testNames {
			fmt.Fprintf(os.Stderr, "%s\n", key)
		}
		return 3
	}
	originalBytes, err := os.ReadFile("/proc/sys/kernel/core_pattern")
	panicIfErr(err)
	originalCorePattern = string(originalBytes)
	setCorePattern(corePattern, alwaysCtx, client)
	defer setCorePattern(originalCorePattern, alwaysCtx, client)
	jobs := make(chan *Test, len(suite))
	tests := make([]*Test, 0, len(suite))

	if options.MountPoint != "" {
		hostConfig.Mounts = []mount.Mount{
			{Type: mount.TypeBind, Source: options.MountPoint, Target: "/saunafs"},
		}
	}

	var wg sync.WaitGroup

	for range options.Workers {
		wg.Add(1)
		go runTest(jobs, &wg)
	}

	for i, testName := range suite {
		if options.SkipUpgradeTests && strings.Contains(testName, "test_upgrade") {
			log.Printf("Skipping test %s", testName)
			continue
		}
		test := &Test{
			Name:          testName,
			TestSuite:     options.Suite,
			Ctx:           ctx,
			Client:        client,
			ContainerName: "sfs-test-" + strconv.Itoa(i+1),
		}
		jobs <- test
		tests = append(tests, test)
	}
	close(jobs)
	wg.Wait()
	if ctx.Err() != nil {
		return 2
	}
	log.Println("All tests finished")

	exitCode := 0
	// Go through the tests twice, to keep things ordered.
	for _, test := range tests {
		if test.Success {
			fmt.Printf("TEST %s: OK\n", test.Name)
			if options.AllOutput {
				fmt.Printf("- %s OUTPUT -\n", test.Name)
				fmt.Printf("%s\n", string(test.TestOutput))
				fmt.Printf("- END OUTPUT -\n")
			}
		}
	}
	for _, test := range tests {
		if !test.Success {
			fmt.Printf("TEST %s: FAILED\n", test.Name)
			fmt.Printf("FAILED IN CONTAINER %s\n", test.ContainerName)
			fmt.Printf("- %s OUTPUT -\n", test.Name)
			fmt.Printf("%s\n", string(test.TestOutput))
			fmt.Printf("- END OUTPUT -\n")
			exitCode = 1
		}
	}
	return exitCode
}

func runTest(jobs <-chan *Test, wg *sync.WaitGroup) {
	defer wg.Done()

	for job := range jobs {
		filter := fmt.Sprintf("%s.%s", job.TestSuite, job.Name)
		gtestFilter := fmt.Sprintf("--gtest_filter=%s", filter)
		var output bytes.Buffer

		setupContainer(job.ContainerName, job.Client, job.Ctx)
		log.Println("Running test: " + job.TestSuite + "/" + job.Name + " in container " + job.ContainerName)

		opts := types.ExecStartCheck{}
		execConfig := types.ExecConfig{
			Cmd:          []string{"saunafs-tests", gtestFilter, "gtest_color=no"},
			AttachStdout: true,
			AttachStderr: true,
			Env:          []string{"COREDUMP_WATCH=1", fmt.Sprintf("SAUNAFS_TEST_TIMEOUT_MULTIPLIER=%d", options.Multiplier)},
		}
		resp, err := job.Client.ContainerExecCreate(job.Ctx, job.ContainerName, execConfig)
		if errdefs.IsCancelled(err) {
			return
		}
		panicIfErr(err)

		attachResp, err := job.Client.ContainerExecAttach(job.Ctx, resp.ID, opts)
		if errdefs.IsCancelled(err) {
			return
		}
		panicIfErr(err)
		_, err = stdcopy.StdCopy(&output, &output, attachResp.Reader)
		if errdefs.IsCancelled(err) {
			return
		}
		panicIfErr(err)

		if bytes.Contains(output.Bytes(), []byte("1 FAILED TEST")) {
			log.Printf("Test %s finished: FAILED", job.Name)
			job.Success = false
			job.TestOutput = output.Bytes()
			if options.DeleteContainers {
				cleanContainer(job.Ctx, job.Client, job.ContainerName)
			}
		} else {
			log.Printf("Test %s finished: OK", job.Name)
			job.Success = true
			cleanContainer(job.Ctx, job.Client, job.ContainerName)
			if options.AllOutput {
				job.TestOutput = output.Bytes()
			}
		}
	}
}

func setupContainer(name string, client *client.Client, ctx context.Context) {
	cont, err := client.ContainerInspect(ctx, name)
	if err == nil {
		log.Printf("Container %s already exists, removing it...\n", name)
		cleanContainer(ctx, client, cont.Name)
	} else if errdefs.IsNotFound(err) {
		log.Printf("Container %s does not exist, creating it...\n", name)
	} else if errdefs.IsCancelled(err) {
		return
	} else {
		panicIfErr(err)
	}

	resp, err := client.ContainerCreate(
		ctx,
		&container.Config{
			Image: imageName,
			Cmd:   []string{"sleep", "infinity"},
			Tty:   true,
			Env:   []string{"TERM=xterm" /* , "DEBUG=1", "DEBUG_LEVEL=5" */},
		},
		&hostConfig,
		nil,
		nil,
		name,
	)
	if errdefs.IsCancelled(err) {
		return
	}
	panicIfErr(err)

	err = client.ContainerStart(ctx, resp.ID, container.StartOptions{})
	if errdefs.IsCancelled(err) {
		return
	}
	panicIfErr(err)

	for {
		container, err := client.ContainerInspect(ctx, resp.ID)
		if errdefs.IsCancelled(err) {
			return
		}
		panicIfErr(err)

		if container.State.Running {
			break
		}

		log.Printf("Waiting for container %s to start...\n", name)
		time.Sleep(1 * time.Second)
	}
}

func cleanContainer(ctx context.Context, client *client.Client, name string) {
	err := client.ContainerStop(ctx, name, container.StopOptions{Signal: "SIGKILL"})
	if errdefs.IsCancelled(err) {
		return
	}
	panicIfErr(err)

	err = client.ContainerRemove(ctx, name, container.RemoveOptions{Force: true})
	if errdefs.IsCancelled(err) {
		return
	}
	panicIfErr(err)
}

// Funny how this works on a """Recommended""" docker setup without requiring
// root privileges
func setCorePattern(original string, ctx context.Context, client *client.Client) {
	const name string = "sfs-tmp"
	setupContainer(name, client, ctx)
	defer cleanContainer(ctx, client, "sfs-tmp")

	execConfig := types.ExecConfig{
		Cmd:          []string{"bash", "-c", "echo \"" + original + "\" > /proc/sys/kernel/core_pattern"},
		AttachStdout: true,
		AttachStderr: true,
	}
	resp, err := client.ContainerExecCreate(ctx, name, execConfig)
	panicIfErr(err)
	respAttach, err := client.ContainerExecAttach(ctx, resp.ID, types.ExecStartCheck{})

	_, err = stdcopy.StdCopy(os.Stdout, os.Stderr, respAttach.Reader)
	panicIfErr(err)
}

func getSuites(ctx context.Context, client *client.Client, containerName string) map[string][]string {
	suites := map[string][]string{}
	execConfig := types.ExecConfig{
		Cmd:          []string{"bash", "-c", "ls -1 /saunafs/tests/test_suites/"},
		AttachStdout: true,
		AttachStderr: true,
	}
	resp, err := client.ContainerExecCreate(ctx, containerName, execConfig)
	panicIfErr(err)
	respAttach, err := client.ContainerExecAttach(ctx, resp.ID, types.ExecStartCheck{})

	var stdout bytes.Buffer

	_, err = stdcopy.StdCopy(&stdout, os.Stderr, respAttach.Reader)
	panicIfErr(err)
	filesLines := bytes.Split(stdout.Bytes(), []byte("\n"))
	for _, bytesLine := range filesLines {
		line := filepath.Base(string(bytesLine))
		if len(line) < 2 {
			continue
		}
		suite, _ := strings.CutSuffix(line, filepath.Ext(line))
		suites[suite] = make([]string, 0, 200)
	}
	return suites
}

func getTestGlobs(ctx context.Context, client *client.Client) map[string][]string {
	const name string = "sfs-tmp"
	setupContainer(name, client, ctx)
	defer cleanContainer(ctx, client, "sfs-tmp")

	suites := getSuites(ctx, client, name)
	for suite := range suites {
		execConfig := types.ExecConfig{
			Cmd:          []string{"bash", "-c", "ls -1 /saunafs/tests/test_suites/" + suite + "/" + options.TestPattern},
			AttachStdout: true,
			AttachStderr: false,
		}
		resp, err := client.ContainerExecCreate(ctx, name, execConfig)
		panicIfErr(err)
		respAttach, err := client.ContainerExecAttach(ctx, resp.ID, types.ExecStartCheck{})

		var stdout bytes.Buffer

		_, err = stdcopy.StdCopy(&stdout, os.Stderr, respAttach.Reader)
		panicIfErr(err)

		filesLines := bytes.Split(stdout.Bytes(), []byte("\n"))
		for _, bytesLine := range filesLines {
			line := filepath.Base(string(bytesLine))
			if len(line) < 2 {
				continue
			}
			test, _ := strings.CutSuffix(line, filepath.Ext(line))
			suites[suite] = append(suites[suite], test)
		}
	}

	return suites
}
