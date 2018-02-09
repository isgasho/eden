#!/usr/bin/env python3
#
# Copyright (c) 2016-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from types import TracebackType
from typing import cast, Dict, Optional, List
from eden.cli import util

import eden.thrift
from fb303.ttypes import fb_status
from .find_executables import EDEN_CLI, EDEN_DAEMON


class EdenFS(object):
    '''Manages an instance of the eden fuse server.'''

    def __init__(
        self,
        eden_dir: Optional[str] = None,
        etc_eden_dir: Optional[str] = None,
        home_dir: Optional[str] = None,
        logging_settings: Optional[Dict[str, str]] = None,
        storage_engine: str = 'memory'
    ) -> None:
        if eden_dir is None:
            eden_dir = tempfile.mkdtemp(prefix='eden_test.')
        self._eden_dir = eden_dir
        self._storage_engine = storage_engine

        self._process: Optional[subprocess.Popen] = None
        self._etc_eden_dir = etc_eden_dir
        self._home_dir = home_dir
        self._logging_settings = logging_settings

    @property
    def eden_dir(self) -> str:
        return self._eden_dir

    def __enter__(self) -> 'EdenFS':
        return self

    def __exit__(
        self, exc_type: type, exc_value: BaseException, tb: TracebackType
    ) -> bool:
        self.cleanup()
        return False

    def cleanup(self) -> None:
        '''Stop the instance and clean up its temporary directories'''
        self.kill()
        self.cleanup_dirs()

    def cleanup_dirs(self) -> None:
        '''Remove any temporary dirs we have created.'''
        shutil.rmtree(self._eden_dir, ignore_errors=True)

    def kill(self) -> None:
        '''Stops and unmounts this instance.'''
        if self._process is None or self._process.returncode is not None:
            return
        self.shutdown()

    def _wait_for_healthy(
        self, timeout: float, exclude_pid: Optional[int]=None
    ) -> None:
        '''Wait for edenfs to start and report that it is healthy.

        Throws an error if it doesn't come up within the specified time.

        If exclude_pid, wait until edenfs reports a process ID different than
        the one specified by exclude_pid.  This allows us to wait until the
        edenfs process changes when performing graceful restart.
        '''
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with self.get_thrift_client() as client:
                    if client.getStatus() == fb_status.ALIVE:
                        if exclude_pid is None:
                            return
                        # Also wait until the PID is different
                        pid = client.getPid()
                        if pid == exclude_pid:
                            print('healthy, but wrong pid (%d)' % exclude_pid,
                                  file=sys.stderr)
                        else:
                            return
            except eden.thrift.EdenNotRunningError as ex:
                pass

            assert self._process is not None
            status = self._process.poll()
            if status is not None:
                if status < 0:
                    msg = 'terminated with signal {}'.format(-status)
                else:
                    msg = 'exit status {}'.format(status)
                raise Exception('edenfs exited before becoming healthy: ' +
                                msg)

            time.sleep(0.1)
        raise Exception("edenfs didn't start within timeout of %s" % timeout)

    def get_thrift_client(self) -> eden.thrift.EdenClient:
        return eden.thrift.create_thrift_client(self._eden_dir)

    def run_cmd(
        self, command: str, *args: str, cwd: Optional[str] = None
    ) -> str:
        '''
        Run the specified eden command.

        Args: The eden command name and any arguments to pass to it.
        Usage example: run_cmd('mount', 'my_eden_client')
        Throws a subprocess.CalledProcessError if eden exits unsuccessfully.
        '''
        cmd = self._get_eden_args(command, *args)
        try:
            completed_process = subprocess.run(cmd, stdout=subprocess.PIPE,
                                               stderr=subprocess.PIPE,
                                               check=True, cwd=cwd)
        except subprocess.CalledProcessError as ex:
            # Re-raise our own exception type so we can include the error
            # output.
            raise EdenCommandError(ex)
        return cast(str, completed_process.stdout.decode('utf-8'))

    def run_unchecked(self, command: str, *args: str) -> int:
        '''
        Run the specified eden command.

        Args: The eden command name and any arguments to pass to it.
        Usage example: run_cmd('mount', 'my_eden_client')
        Returns the process return code.
        '''
        cmd = self._get_eden_args(command, *args)
        return subprocess.call(cmd)

    def _get_eden_args(self, command: str, *args: str) -> List[str]:
        '''Combines the specified eden command args with the appropriate
        defaults.

        Args:
            command: the eden command
            *args: extra string arguments to the command
        Returns:
            A list of arguments to run Eden that can be used with
            subprocess.Popen() or subprocess.check_call().
        '''
        cmd = [EDEN_CLI, '--config-dir', self._eden_dir]
        if self._etc_eden_dir:
            cmd += ['--etc-eden-dir', self._etc_eden_dir]
        if self._home_dir:
            cmd += ['--home-dir', self._home_dir]
        cmd.append(command)
        cmd.extend(args)
        return cmd

    def start(
        self, timeout: float=60, takeover_from: Optional[int]=None
    ) -> None:
        '''
        Run "eden daemon" to start the eden daemon.
        '''
        use_gdb = False
        if os.environ.get('EDEN_GDB'):
            use_gdb = True
            # Starting up under GDB takes longer than normal.
            # Allow an extra 90 seconds (for some reason GDB can take a very
            # long time to load symbol information, particularly on dynamically
            # linked builds).
            timeout += 90

        takeover = (takeover_from is not None)
        self._spawn(gdb=use_gdb, takeover=takeover)

        self._wait_for_healthy(timeout, exclude_pid=takeover_from)

    def _spawn(self, gdb: bool = False, takeover: bool = False) -> None:
        if self._process is not None:
            raise Exception('cannot start an already-running eden client')

        args = self._get_eden_args(
            'daemon',
            '--daemon-binary', EDEN_DAEMON,
            '--foreground',
        )

        extra_daemon_args = [
            '--',
            # Defaulting to 8 import processes is excessive when the test
            # framework runs tests on each CPU core.
            '--num_hg_import_threads', '2',
            '--local_storage_engine_unsafe', self._storage_engine,
        ]
        if 'SANDCASTLE' in os.environ:
            extra_daemon_args.append('--allowRoot')

        if takeover:
            args.append('--takeover')

        # If the EDEN_GDB environment variable is set, run eden inside gdb
        # so a developer can debug crashes
        if os.environ.get('EDEN_GDB'):
            gdb_exit_handler = (
                'python gdb.events.exited.connect('
                'lambda event: '
                'gdb.execute("quit") if getattr(event, "exit_code", None) == 0 '
                'else False'
                ')'
            )
            gdb_args = [
                # Register a handler to exit gdb if the program finishes
                # successfully.
                # Start the program immediately when gdb starts
                '-ex', gdb_exit_handler,
                # Start the program immediately when gdb starts
                '-ex', 'run'
            ]
            args.append('--gdb')
            for arg in gdb_args:
                args.append('--gdb-arg=' + arg)

        # Turn up the VLOG level for the fuse server so that errors are logged
        # with an explanation when they bubble up to RequestData::catchErrors
        if self._logging_settings:
            logging_arg = ','.join('%s=%s' % (module, level)
                                   for module, level in sorted(
                                       self._logging_settings.items()))
            extra_daemon_args.extend(['--logging=' + logging_arg])
        if 'EDEN_DAEMON_ARGS' in os.environ:
            args.extend(shlex.split(os.environ['EDEN_DAEMON_ARGS']))

        self._process = subprocess.Popen(args + extra_daemon_args)

    def shutdown(self) -> None:
        '''
        Run "eden shutdown" to stop the eden daemon.
        '''
        assert self._process is not None

        # We need to take care here: the normal `eden shutdown` command will
        # wait for eden to successfully finish by repeatedly testing to see
        # whether the process is alive.  However, running as root we don't
        # spawn an intermediate process and this results in our child process
        # (attached to self._process) to land in a defunct state such that
        # the `kill(pid, 0)` test in the `eden shutdown` command still considers
        # the process alive.   To avoid this situation we ask the shutdown
        # command not to wait and instead perform our own polling here against
        # the real process handle.
        self.run_cmd('shutdown', '-t', '0')
        util.poll_until(lambda: self._process.poll(), timeout=15)
        return_code = self._process.wait()
        self._process = None
        if return_code != 0:
            raise Exception('eden exited unsuccessfully with status {}'.format(
                return_code))

    def graceful_restart(self, timeout: float = 30) -> None:
        assert self._process is not None
        # Get the process ID of the old edenfs process.
        # Note that this is not necessarily self._process.pid, since the eden
        # CLI may have spawned eden using sudo, and self._process may refer to
        # a sudo parent process.
        with self.get_thrift_client() as client:
            old_pid = client.getPid()

        old_process = self._process
        self._process = None

        self.start(timeout=timeout, takeover_from=old_pid)

        # Check the return code from the old edenfs process
        return_code = old_process.wait()
        if return_code != 0:
            raise Exception('eden exited unsuccessfully with status {}'.format(
                return_code))

    def add_repository(self, name: str, repo_path: str) -> None:
        '''
        Run "eden repository" to define a repository configuration
        '''
        self.run_cmd('repository', name, repo_path)

    def repository_cmd(self) -> str:
        '''
        Executes "eden repository" to list the repositories,
        and returns the output as a string.
        '''
        return self.run_cmd('repository')

    CLIENT_ACTIVE = 'active'
    CLIENT_INACTIVE = 'inactive'
    CLIENT_UNCONFIGURED = 'unconfigured'

    def list_cmd(self) -> Dict[str, str]:
        '''
        Executes "eden list" to list the client directories,
        and returns the output as a dictionary of { client_path -> status }

        The status can be one of the CLIENT_ACTIVE, CLIENT_INACTIVE, or
        CLIENT_UNCONFIGURED constants.
        'active', 'inactive', or 'unconfigured'
        '''
        lines = self.run_cmd('list').splitlines()

        results = {}
        active_suffix = ' (active)'
        unconfigured_suffix = ' (unconfigured)'
        for line in lines:
            if line.endswith(active_suffix):
                path = line[:-len(active_suffix)]
                status = self.CLIENT_ACTIVE
            elif line.endswith(unconfigured_suffix):
                path = line[:-len(active_suffix)]
                status = self.CLIENT_UNCONFIGURED
            else:
                path = line
                status = self.CLIENT_INACTIVE
            results[path] = status

        return results

    def clone(self, repo: str, path: str, allow_empty: bool = False) -> None:
        '''
        Run "eden clone"
        '''
        params = ['clone', repo, path]
        if allow_empty:
            params.append('--allow-empty-repo')
        self.run_cmd(*params)

    def unmount(self, path: str) -> None:
        '''
        Run "eden unmount --destroy <path>"
        '''
        self.run_cmd('unmount', '--destroy', path)

    def in_proc_mounts(self, mount_path: str) -> bool:
        '''Gets all eden mounts found in /proc/mounts, and returns
        true if this eden instance exists in list.
        '''
        with open('/proc/mounts', 'r') as f:
            mounts = [line.split(' ')[1] for line in f.readlines()
                      if line.split(' ')[0] == 'edenfs']
        return mount_path in mounts

    def is_healthy(self) -> bool:
        '''Executes `eden health` and returns True if it exited with code 0.'''
        return_code = self.run_unchecked('health')
        return return_code == 0


class EdenCommandError(subprocess.CalledProcessError):
    def __init__(self, ex: subprocess.CalledProcessError) -> None:
        super().__init__(ex.returncode, ex.cmd, output=ex.output,
                         stderr=ex.stderr)

    def __str__(self) -> str:
        return ("eden command '%s' returned non-zero exit status %d\n"
                "stderr=%s" % (self.cmd, self.returncode, self.stderr))


_can_run_eden: Optional[bool] = None


def can_run_eden() -> bool:
    '''
    Determine if we can run eden.

    This is used to determine if we should even attempt running the
    integration tests.
    '''
    global _can_run_eden
    if _can_run_eden is None:
        _can_run_eden = _compute_can_run_eden()

    return _can_run_eden


def _compute_can_run_eden() -> bool:
    # FUSE must be available
    if not os.path.exists('/dev/fuse'):
        return False

    # We must be able to start eden as root.
    if os.geteuid() == 0:
        return True

    # The daemon must either be setuid root, or we must have sudo priviliges.
    # Typically for the tests the daemon process is not setuid root,
    # so check if we have are able to run things under sudo.
    return _can_run_sudo()


def _can_run_sudo() -> bool:
    cmd = ['/usr/bin/sudo', '-E', '/bin/true']
    with open('/dev/null', 'r') as dev_null:
        # Close stdout, stderr, and stdin, and call setsid() to make
        # sure we are detached from any controlling terminal.  This makes
        # sure that sudo can't prompt for a password if it needs one.
        # sudo will only succeed if it can run with no user input.
        process = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, stdin=dev_null,
                                   preexec_fn=os.setsid)
    process.communicate()
    return process.returncode == 0
