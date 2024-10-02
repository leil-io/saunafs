pipeline {
    agent { label 'ubuntu-2404' }
    options {
        skipStagesAfterUnstable()
        // TODO: Add more lenient rules for dev
        buildDiscarder(logRotator(artifactDaysToKeepStr: "7", artifactNumToKeepStr: "2"))
    }
    tools { go '1.22.2' }
    stages {
        stage('Checkout code') {
            steps {
                checkout scm
            }
        }

        stage('Build sfstests') {
            steps {
                sh '''
                    git clone "https://github.com/leil-io/sfstests"
                    cd sfstests
                    git checkout v0.2.1
                    go build -o $WORKSPACE/sfstests
                    '''
            }
        }

        stage('Build image') {
            steps {
                sh '''
                    cd $WORKSPACE
                    mkdir build
                    docker buildx build --build-arg BASE_IMAGE=ubuntu:24.04 --tag saunafs-test:latest -f sfstests/Dockerfile.ci $WORKSPACE
                    docker save saunafs-test:latest -o ./build/sfstests.tar
                    '''
                archiveArtifacts artifacts: 'build/*.tar', fingerprint: true
            }
        }

        stage('Run Sanity') {
            steps {
                sh ''' ./sfstests/sfstests --workers 28 --cpus 1'''
            }
        }

        stage('Run short system tests') {
            steps {
                sh ''' ./sfstests/sfstests --suite ShortSystemTests --workers 16 --multiplier 2 --cpus 2'''
            }
        }

        stage('Run long system tests') {
            steps {
                sh ''' ./sfstests/sfstests --suite LongSystemTests --workers 12 --multiplier 2 --cpus 2'''
            }
        }
    }
    post {
        // Clean after build
        always {
            cleanWs(cleanWhenNotBuilt: true,
                deleteDirs: true,
                disableDeferredWipeout: true,
                notFailBuild: true,
            )
            sh '''
                docker rm $(docker stop $(docker ps -a -q --filter ancestor=saunafs-test --format="{{.ID}}")) || true
                docker image rm saunafs-test || true
                '''
        }
    }
}
