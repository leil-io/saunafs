def imageTags = [:]
pipeline {
    agent {
        label 'docker'
    }
    environment{
        PROJECT_NAME = 'saunafs'
        DOCKER_INTERNAL_REGISTRY = "registry.ci.leil.io"
        DOCKER_ENABLE_PUSH_IMAGE = 'true'
        DOCKER_ENABLE_PULL_CACHE_IMAGE = 'true'
        DOCKER_INTERNAL_REGISTRY_URL= "https://${DOCKER_INTERNAL_REGISTRY}"
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
        stage('Preparation') {
            parallel {
                stage('Image_Build') {
                    agent { label 'docker' }
                    steps {
                        cleanAndClone()
                        script {
                            docker.withRegistry(env.DOCKER_INTERNAL_REGISTRY_URL, env.dockerRegistrySecretId) {
                                sh "tests/ci_build/docker-build.sh ubuntu22.04-test"
                            }
                            imageTags['ubuntu22.04-test'] = getPartialTag(env.BRANCH_NAME, 'ubuntu22.04-test', getCommitId())
                        }
                    }
                }
            }
        }
        stage('Build') {
            parallel {
                stage ('BuildProject') {
                    agent {
                        docker {
                            label 'docker'
                            image "${DOCKER_INTERNAL_REGISTRY}/${PROJECT_NAME}:" + imageTags['ubuntu22.04-test']
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
                stage('GaneshaImage') {
                    when {
                        beforeAgent true
                        expression { env.BRANCH_NAME ==~ /(main|dev)/ }
                    }
                    agent { label 'docker' }
                    steps {
                        cleanAndClone()
                        script {
                            docker.withRegistry(env.DOCKER_INTERNAL_REGISTRY_URL, env.dockerRegistrySecretId) {
                                DOCKER_BASE_IMAGE="${DOCKER_INTERNAL_REGISTRY}/${PROJECT_NAME}:" + imageTags['ubuntu22.04-test']
                                sh "tests/ci_build/docker-build.sh ganesha"
                            }
                            imageTags['ganesha'] = getPartialTag(env.BRANCH_NAME, 'ganesha', 'latest')
                        }
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
                            image "${DOCKER_INTERNAL_REGISTRY}/${PROJECT_NAME}:" + imageTags['ubuntu22.04-test']
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
                            image "${DOCKER_INTERNAL_REGISTRY}/${PROJECT_NAME}:" + imageTags['ubuntu22.04-test']
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
                        expression { env.BRANCH_NAME ==~ /(main|dev)/ }
                    }
                    agent {
                        docker {
                            label 'docker'
                            image "${DOCKER_INTERNAL_REGISTRY}/${PROJECT_NAME}:" + imageTags['ganesha']
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
        stage('Package') {
            when {
                beforeAgent true
                expression { env.BRANCH_NAME ==~ /(main|dev)/ }
            }
            agent {
                docker {
                    label 'docker'
                    image "${DOCKER_INTERNAL_REGISTRY}/${PROJECT_NAME}:" + imageTags['ubuntu22.04-test']
                    registryUrl env.DOCKER_INTERNAL_REGISTRY_URL
                    registryCredentialsId env.dockerRegistrySecretId
                    args  '--security-opt seccomp=unconfined'
                }
            }
            steps {
                cleanAndClone()
                script {
                    sh "./package.sh"
                    stash allowEmpty: false, name: 'bundle', includes: "*bundle*.tar"
                    archiveArtifacts artifacts: '*bundle*.tar', followSymlinks: false
                }
            }
        }
        stage('Delivery') {
            when {
                beforeAgent true
                expression { env.BRANCH_NAME ==~ /(main|dev)/ }
            }
            agent {
                docker {
                    label 'docker'
                    image "ictus4u/ssh-courier:1.1.1"
                }
            }
            steps {
                cleanAndClone()
                script {
                    unstash 'bundle'
                    withCredentials([usernamePassword(credentialsId: 'nexus-deployment-credentials', passwordVariable: 'nexusDeploymentPassword', usernameVariable: 'nexusDeploymentUsername')]) {
                        withEnv([
                            "NEXUS_USERNAME=${nexusDeploymentUsername}",
                            "NEXUS_PASSWORD=${nexusDeploymentPassword}",
                            "NEXUS_URL=${env.nexusUrl}"
                        ]) {
                            sh '''
                                bundle_name=$(ls -1t *bundle*.tar | head -1)
                                nexusRepoName="${PROJECT_NAME}-$(echo "${bundle_name}" | cut -d- -f3-4)"
                                if [ "${BRANCH_NAME}" != "main" ]; then
                                    nexusRepoName="${nexusRepoName}-${BRANCH_NAME}"
                                fi
                                tar -vxf ${bundle_name}
                                bundle_dir=$(basename ${bundle_name} .tar)
                                NEXUS_REPO_NAME=${nexusRepoName} \
                                    bash -x tests/ci_build/run-delivery-nexus-deb.sh \
                                        ${bundle_dir}
                            '''
                        }
                    }
                }
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
