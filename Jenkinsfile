@Library('leil-ci-utils') _

pipeline {
    agent none
    options {
        skipDefaultCheckout()
        timestamps()
        ansiColor("xterm")
        parallelsAlwaysFailFast()
        preserveStashes(buildCount: 2)
    }
    stages {
        stage('Build Images') {
            when {
                beforeAgent true
                expression { env.BRANCH_NAME == 'ci-containers' }
            }
            steps {
                build job: 'saunafs_container_matrix', parameters: [
                    string(name: 'SAUNAFS_REF', value: env.BRANCH_NAME)
                ]
            }
        }
        stage('Test') {
            steps {
                build job: 'saunafs_tests', parameters: [
                    string(name: 'SAUNAFS_REF', value: env.BRANCH_NAME)
                ]
            }
        }
    }
    post {
        always {
            sendNotifications currentBuild.result
        }
    }
}

