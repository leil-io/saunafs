#
# To run this test you need to add the following lines to /etc/sudoers.d/saunafstest:
#
# saunafstest ALL = NOPASSWD: /usr/bin/tee /tmp/SaunaFS-autotests/mnt/sfs*
#
#
MOUNTS=2 \
  USE_RAMDISK=YES \
  MOUNT_0_EXTRA_CONFIG="sfsdirectio=0,sfsignoreflush=0" \
  MOUNT_1_EXTRA_CONFIG="sfsdirectio=1,sfsignoreflush=1" \
  setup_local_empty_saunafs info

expect_equals "false" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i directio | awk '{print $2}')"
expect_equals "false" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i ignoreflush | awk '{print $2}')"
expect_equals "true" "$(cat ${info[mount1]}/.saunafs_tweaks | grep -i directio | awk '{print $2}')"
expect_equals "true" "$(cat ${info[mount1]}/.saunafs_tweaks | grep -i ignoreflush | awk '{print $2}')"

# Modify the options on the fly from tweaks file
echo "DirectIO=true" | sudo tee ${info[mount0]}/.saunafs_tweaks
echo "IgnoreFlush=true" | sudo tee ${info[mount0]}/.saunafs_tweaks
echo "DirectIO=false" | sudo tee ${info[mount1]}/.saunafs_tweaks
echo "IgnoreFlush=false" | sudo tee ${info[mount1]}/.saunafs_tweaks

expect_equals "true" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i directio | awk '{print $2}')"
expect_equals "true" "$(cat ${info[mount0]}/.saunafs_tweaks | grep -i ignoreflush | awk '{print $2}')"
expect_equals "false" "$(cat ${info[mount1]}/.saunafs_tweaks | grep -i directio | awk '{print $2}')"
expect_equals "false" "$(cat ${info[mount1]}/.saunafs_tweaks | grep -i ignoreflush | awk '{print $2}')"
