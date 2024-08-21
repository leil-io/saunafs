# sfstests - A way to quickly run SaunaFS tests locally on any machine

## Rationale

SaunaFS testing framework is quite powerful, however it has significant
drawbacks, two of which are developing on non-Ubuntu machines (and tests
requiring significant modification to the local machine). It also has the problem
of tests taking too long, with ShortSystemTests taking up to an hour, and
LongSystemTests taking an entire weekend! While the CI had a solution for the
latter problem, it required committing/pushing/waiting and isn't ideal for a
local development environment of quickly iterating.

This tool solves the above problems by instead creating and managing docker
containers to run tests instead, without requiring any system modification or
being on Ubuntu. It also runs tests in parallel, reducing the amount of time
from running them consecutively by potentially 20 times! (depends on processor
cores)

## Docker

A docker image is provided to help build the image used. Currently, the tool
doesn't build it, so you'll have to manually build it with something like this
(assuming working directory is saunafs source directory):

```bash
docker buildx build --tag saunafs-test:latest -f tests/sfstests/Dockerfile.test .
```

**Note the tag used is a requirement**, the tool uses this to find the image to be
used (it could be parameterized in the future)

You also need to setup docker so you can run it as a normal user, or use sudo
instead for the commands below after building and installing.

## Building and installing

Obviously you need Go setup, see their webpage/your local package manager for
more details.

You can run the command directly with `go run`. However you should probably
install it, in which case you should use `go install`.

```bash
go install
```

This will put the binary in $GOPATH/, which will likely you not be in
your PATH variable. You can add it or use this command below to specify where to
put the binary exactly

```bash
GOBIN=/usr/local/bin/ go install
```

## Usage

```bash
sfstests -h
```

## Performance

Performance will depend on the number of containers, or "workers". Each of these
workers will take a test to run as long as there are still remaining tests to
run in a suite. However, more workers equals more RAM/CPU usage, so unless you
have some 100GB of RAM, you might want to balance things here. By default, the
application uses the number of logical cores your CPU has, but this can be
increased by using the -w option

```bash
sfstests --workers 50 # Run 50 workers
```
