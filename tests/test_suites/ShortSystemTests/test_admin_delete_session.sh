# Set up saunafs environment
MOUNTS=1
USE_RAMDISK=YES
setup_local_empty_saunafs info

cd "${info[mount0]}"

echo "Alicia starts testing this program." > alicia.txt

echo "List of sessions before test:"
saunafs-admin list-sessions localhost "${info[matocl]}"

sessions=$(saunafs-admin list-sessions localhost "${info[matocl]}")
num_files=$(grep "Open files: " <<< "$sessions" | sed 's/^.*: //')

echo "Number of open files before test: $num_files"

expected_file_count=1

# Check if the number of open files decreased by 1 using expect_equals
expect_equals "$expected_file_count" "$num_files" "Integration test: Session count"

# Run the delete-session command to delete a session
saunafs-admin delete-session localhost "${info[matocl]}" 1

# Get the updated count of sessions
updated_file_count=$(saunafs-admin list-sessions localhost "${info[matocl]}")
open_files_line=$(echo "$updated_file_count" | grep "Open files: ")
num_files_after=$(echo "$open_files_line" | sed 's/Open files: //')

echo "Number of open files after test: $num_files_after"
echo "List of sessions after test:"
saunafs-admin list-sessions localhost "${info[matocl]}"

expected_updated_file_count=0

# Check if the number of open files decreased by 1 using expect_equals
expect_equals "$expected_updated_file_count" "$num_files_after" "Integration test: Session count"
