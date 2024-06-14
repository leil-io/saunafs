def imageTags = [:]
pipeline {
    agent { label 'docker' }
    environment{
        PROJECT_NAME = 'saunafs'
        DOCKER_INTERNAL_REGISTRY = "registry.ci.leil.io"
        DOCKER_INTERNAL_REGISTRY_URL= "https://${DOCKER_INTERNAL_REGISTRY}"
        dockerImageSaunafs = 'registry.ci.leil.io/saunafs-test:dev-ubuntu-24.04-latest'
        dockerImageSaunafsGanesha = 'registry.ci.leil.io/saunafs-test:dev-ganesha-ubuntu-24.04-latest'
        dockerRegistrySecretId = 'private-docker-registry-credentials'
        nexusUrl = "http://192.168.50.208:8081"
    }
    options {
        skipDefaultCheckout()
        timestamps()
        ansiColor("xterm")
        parallelsAlwaysFailFast()
        preserveStashes(buildCount: 2)
    }
    stages {
        stage('Build') {
            parallel {
                stage ('BuildProject') {
                    agent {
                        docker {
                            label 'docker'
                            image env.dockerImageSaunafs
                            registryUrl env.DOCKER_INTERNAL_REGISTRY_URL
                            registryCredentialsId env.dockerRegistrySecretId
                            args  '--security-opt seccomp=unconfined --cap-add SYS_ADMIN --device=/dev/fuse:/dev/fuse --security-opt="apparmor=unconfined" --tmpfs /mnt/ramdisk:rw,mode=1777,size=2g --ulimit core=-1'
                        }
                    }
                    steps {
                        cleanAndClone()
                        sh 'tests/ci_build/run-build.sh test'
                        stash allowEmpty: true, name: 'test-binaries', includes: "install/saunafs/**/*"
                        stash allowEmpty: true, name: 'built-binaries', includes: "build/saunafs/**/*"
                    }
                }
            }
        }
        stage('Tests') {
            parallel {
                stage("CppCheck") {
                    agent {
                        docker {
                            label 'docker'
                            image "registry.ci.leil.io/ci-tools/cppcheck:2023.09.18"
                            registryUrl 'https://registry.ci.leil.io'
                            registryCredentialsId env.dockerRegistrySecretId
                        }
                    }
                    steps {
                        cleanAndClone()
                        sh 'cppcheck --enable=all --inconclusive --xml --xml-version=2 ./src 2> cppcheck.xml'
                        archiveArtifacts artifacts: 'cppcheck.xml', followSymlinks: false
                        recordIssues enabledForFailure: true, tool: cppCheck(name: "Lint: cppcheck", pattern: 'cppcheck.xml')
                    }
                }
                stage("CppLint") {
                    agent {
                        docker {
                            label 'docker'
                            image "registry.ci.leil.io/ci-tools/cpplint:2023.09.18"
                            registryUrl 'https://registry.ci.leil.io'
                            registryCredentialsId env.dockerRegistrySecretId
                        }
                    }
                    steps {
                        cleanAndClone()
                        sh 'cpplint --quiet --counting=detailed --linelength=120 --recursive src/ 2> cpplint.log || true'
                        archiveArtifacts artifacts: 'cpplint.log', followSymlinks: false
                        recordIssues enabledForFailure: true, tool: cppLint(name: "Lint: cpplint", pattern: 'cpplint.log')
                    }
                }
                stage ('SanityCheck') {
                    agent {
                        docker {
                            label 'docker'
                            image env.dockerImageSaunafs
                            registryUrl env.DOCKER_INTERNAL_REGISTRY_URL
                            registryCredentialsId env.dockerRegistrySecretId
                            args  '--security-opt seccomp=unconfined --cap-add SYS_ADMIN --device=/dev/fuse:/dev/fuse --security-opt="apparmor=unconfined" --tmpfs /mnt/ramdisk:rw,mode=1777,size=2g --ulimit core=-1'
                        }
                    }
                    steps {
                        cleanAndClone()
                        unstash 'test-binaries'
                        sh 'tests/ci_build/run-sanity-check.sh'
                    }
                }
                stage('Unit') {
                    agent {
                        docker {
                            label 'docker'
                            image env.dockerImageSaunafs
                            registryUrl env.DOCKER_INTERNAL_REGISTRY_URL
                            registryCredentialsId env.dockerRegistrySecretId
                            args  '--security-opt seccomp=unconfined --cap-add SYS_ADMIN --device=/dev/fuse:/dev/fuse --security-opt="apparmor=unconfined" --tmpfs /mnt/ramdisk:rw,mode=1777,size=2g --ulimit core=-1'
                        }
                    }
                    steps {
                        cleanAndClone()
                        unstash 'built-binaries'
                        unstash 'test-binaries'
                        sh 'tests/ci_build/run-unit-tests.sh'
                    }
                }
                stage('Ganesha') {
                    when {
                        beforeAgent true
                        expression { env.BRANCH_NAME ==~ /(main|dev|PR-.*)/ }
                    }
                    agent {
                        docker {
                            label 'docker'
                            image env.dockerImageSaunafsGanesha
                            registryUrl env.DOCKER_INTERNAL_REGISTRY_URL
                            registryCredentialsId env.dockerRegistrySecretId
                            args  '--security-opt seccomp=unconfined --device=/dev/fuse:/dev/fuse --security-opt="apparmor=unconfined" --tmpfs /mnt/ramdisk:rw,mode=1777,size=2g --ulimit core=-1 --cap-add SYS_ADMIN --cap-add DAC_READ_SEARCH --cap-add SETPCAP'
                        }
                    }
                    steps {
                        cleanAndClone()
                        unstash 'test-binaries'
                        sh 'tests/ci_build/run-ganesha-tests.sh'
                    }
                }
            }
        }
        stage('ShortSystemTests') {
            agent {
                docker {
                    label 'docker'
                    image env.dockerImageSaunafs
                    registryUrl env.DOCKER_INTERNAL_REGISTRY_URL
                    registryCredentialsId env.dockerRegistrySecretId
                    args  '--security-opt seccomp=unconfined --cap-add SYS_ADMIN --device=/dev/fuse:/dev/fuse --security-opt="apparmor=unconfined" --tmpfs /mnt/ramdisk:rw,mode=1777,size=2g --ulimit core=-1'
                }
            }
            steps {
                cleanAndClone()
                unstash 'test-binaries'
                sh 'tests/ci_build/run-sanity-check.sh "ShortSystemTests*"'
            }
        }
        stage('Delivery') {
            when {
                beforeAgent true
                expression { env.BRANCH_NAME ==~ /(main|dev)/ }
            }
            steps {
                cleanAndClone()
                build job: 'saunafs_deliver', parameters: [string(name: 'BRANCH_NAME', value: env.BRANCH_NAME)]
            }
        }
    }
    post {
        always {
            cleanWs()
        }
    }
}

def cleanAndClone() {
    cleanWs()
    checkout scm
}

def getCommitId() {
    return sh(returnStdout: true, script: 'git rev-parse --short HEAD').trim()
}

def getPartialTag(branchName, customTag, commitId = 'latest') {
    return "${branchName}-${customTag}-${commitId}"
}
