# The daemon (docs/design/daemon.md): framing, root discovery, admission, the file table and the
# platform watcher. The process-level tests are tests/daemon.
ycxxc_unit_test(daemon daemon_test.cpp ycxxc_daemon)
