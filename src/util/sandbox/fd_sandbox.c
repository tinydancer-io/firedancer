#if !defined(__linux__)
# error "Target operating system is unsupported by seccomp."
#endif

#define _GNU_SOURCE

#include "fd_sandbox.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <linux/audit.h>
#include <linux/securebits.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/capability.h>
#include <dirent.h>
#include <limits.h>
#include <sched.h>        /* CLONE_*, setns, unshare */
#include <stddef.h>
#include <stdlib.h>       /* clearenv, mkdtemp*/
#include <sys/mount.h>    /* MS_*, MNT_*, mount, umount2 */
#include <sys/prctl.h>
#include <sys/resource.h> /* RLIMIT_*, rlimit, setrlimit */
#include <sys/stat.h>     /* mkdir */
#include <sys/syscall.h>  /* SYS_* */
#include <unistd.h>       /* set*id, sysconf, close, chdir, rmdir syscall */

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define X32_SYSCALL_BIT 0x40000000

#if defined(__i386__)
# define ARCH_NR  AUDIT_ARCH_I386
#elif defined(__x86_64__)
# define ARCH_NR  AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
# define ARCH_NR AUDIT_ARCH_AARCH64
#else
# error "Target architecture is unsupported by seccomp."
#endif

#define FD_TESTV(c) do { if( FD_UNLIKELY( !(c) ) ) FD_LOG_ERR(( "FAIL: (%d:%s) %s", errno, strerror( errno ), #c )); } while(0)

static void
secure_clear_environment( void ) {
  char** env = environ;
  while ( *env ) {
    size_t len = strlen( *env );
    explicit_bzero( *env, len );
    env++;
  }
  clearenv();
}

static void
setup_mountns( void ) {
  FD_TESTV( unshare ( CLONE_NEWNS ) == 0 );

  char temp_dir [] = "/tmp/fd-sandbox-XXXXXX";
  FD_TESTV( NULL != mkdtemp( temp_dir ) );
  FD_TESTV( 0 == mount( NULL, "/", NULL, MS_SLAVE | MS_REC, NULL ) );
  FD_TESTV( 0 == mount( temp_dir, temp_dir, NULL, MS_BIND | MS_REC, NULL ) );
  FD_TESTV( 0 == chdir( temp_dir ) );
  FD_TESTV( 0 == mkdir( "old-root", S_IRUSR | S_IWUSR ));
  FD_TESTV( 0 == syscall( SYS_pivot_root, ".", "old-root" ) );
  FD_TESTV( 0 == chdir( "/" ) );
  FD_TESTV( 0 == umount2( "old-root", MNT_DETACH ) );
  FD_TESTV( 0 == rmdir( "old-root" ) );
}

#define CLOSE_RANGE_UNSHARE (1U << 1)

static void
close_fds_proc( uint close_from ) {
  DIR * dir = opendir( "/proc/self/fd" );
  FD_TESTV( NULL != dir );
  int dirfd1 = dirfd( dir );
  FD_TESTV( dirfd1 >= 0 );

  struct dirent *dp;
  while( ( dp = readdir( dir ) ) != NULL ) {
    char *end;
    long fd = strtol( dp->d_name, &end, 10 );
    FD_TESTV( fd < INT_MAX && fd > INT_MIN );
    if ( *end != '\0' ) {
      continue;
    }

    if ( fd >= close_from && fd != dirfd1 ) {
      FD_TESTV( 0 == close( (int)fd ) );
    }
  }

  FD_TESTV( 0 == closedir( dir ) );
}

// Older kernels might not have SYS_close_range
#if FD_HAS_X86
#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#endif

static void
close_fds( uint close_from ) {
  if( syscall( SYS_close_range, close_from, UINT_MAX, CLOSE_RANGE_UNSHARE ) ) {
    // No SYS_close_range, close one by one
    FD_TESTV( errno == ENOSYS );
    close_fds_proc( close_from );
  }

  /* always close stdin and stdout */
  if( close_from > 0 ) FD_TESTV( 0 == close( 0 ) );
  if( close_from > 1 ) FD_TESTV( 0 == close( 1 ) );
}

static void
install_seccomp( ushort allow_syscalls_sz, long * allow_syscalls ) {
  FD_TEST( allow_syscalls_sz < 32 - 5 );

  struct sock_filter filter [ 128 ] = {
    /* validate architecture, load the arch number */
    BPF_STMT( BPF_LD | BPF_W | BPF_ABS, ( offsetof( struct seccomp_data, arch ) ) ),

    /* do not jump (and die) if the compile arch is neq the runtime arch.
       Otherwise, jump over the SECCOMP_RET_KILL_PROCESS statement. */
    BPF_JUMP( BPF_JMP | BPF_JEQ | BPF_K, ARCH_NR, 1, 0 ),
    BPF_STMT( BPF_RET | BPF_K, SECCOMP_RET_ALLOW ),

    /* verify that the syscall is allowed, oad the syscall */
    BPF_STMT( BPF_LD | BPF_W | BPF_ABS, ( offsetof( struct seccomp_data, nr ) ) ),
  };

  for( ulong i=0; i<allow_syscalls_sz; i++ ) {
    /* If the syscall does not match, jump over RET_ALLOW */
    struct sock_filter jmp = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint)allow_syscalls[i], 0, 1);
    struct sock_filter ret = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    filter[ 4+(i*2)   ] = jmp;
    filter[ 4+(i*2)+1 ] = ret;
  }

  /* none of the syscalls approved were matched: die */
  struct sock_filter kill = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
  filter[ 4+(allow_syscalls_sz*2) ] = kill;

  FD_TESTV( 0 == prctl( PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0 ) );

  struct sock_fprog default_prog = {
    .len = (ushort)(5+(allow_syscalls_sz*2)),
    .filter = filter,
  };
  FD_TESTV( 0 == syscall( SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &default_prog ) );
}

static void
drop_capabilities( void ) {
  FD_TESTV( 0 == prctl (
    PR_SET_SECUREBITS,
    SECBIT_KEEP_CAPS_LOCKED | SECBIT_NO_SETUID_FIXUP |
      SECBIT_NO_SETUID_FIXUP_LOCKED | SECBIT_NOROOT |
      SECBIT_NOROOT_LOCKED | SECBIT_NO_CAP_AMBIENT_RAISE |
      SECBIT_NO_CAP_AMBIENT_RAISE_LOCKED ) );

  struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
  struct __user_cap_data_struct   data[2] = { { 0 } };
  FD_TESTV( 0 == syscall( SYS_capset, &hdr, data ) );

  FD_TESTV( 0 == prctl( PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0 ) );
}

static void userns_map( uint id, const char * map ) {
  char path[64];
  FD_TESTV( sprintf( path, "/proc/self/%s", map ) > 0);
  int fd = open( path, O_WRONLY );
  FD_TESTV( fd >= 0 );
  char line[64];
  FD_TESTV( sprintf( line, "0 %u 1\n", id ) > 0 );
  FD_TESTV( write( fd, line, strlen( line ) ) > 0 );
  FD_TESTV( 0 == close( fd ) );
}

static void deny_setgroups( void ) {
  int fd = open( "/proc/self/setgroups", O_WRONLY );
  FD_TESTV( fd >= 0 );
  FD_TESTV( write( fd, "deny", strlen( "deny" ) ) > 0 );
  FD_TESTV( 0 == close( fd ) );
}

static void unshare_user( uint uid, uint gid ) {
  FD_TESTV( 0 == setresgid( gid, gid, gid ) );
  FD_TESTV( 0 == setresuid( uid, uid, uid ) );
  FD_TESTV( 0 == prctl( PR_SET_DUMPABLE, 1, 0, 0, 0 ) );

  FD_TESTV( 0 == unshare( CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET ) );
  deny_setgroups();
  userns_map( uid, "uid_map" );
  userns_map( gid, "gid_map" );

  FD_TESTV( 0 == prctl( PR_SET_DUMPABLE, 0, 0, 0, 0 ) );
  for ( int cap = 0; cap <= CAP_LAST_CAP; cap++ ) {
    FD_TESTV( 0 == prctl( PR_CAPBSET_DROP, cap, 0, 0, 0 ) );
  }
}

void
fd_sandbox_private_unthreaded( uint uid, uint gid ) {
  unshare_user( uid, gid );
  setup_mountns();
  drop_capabilities();
  secure_clear_environment();
}

void
fd_sandbox_private_threaded( uint close_fd_start,
                             ushort allow_syscalls_sz,
                             long * allow_syscalls ) {
  fd_sandbox_private_no_seccomp( close_fd_start );
  install_seccomp( allow_syscalls_sz, allow_syscalls );
}

void
fd_sandbox( uint uid,
            uint gid,
            uint close_fd_start,
            ushort allow_syscalls_sz,
            long * allow_syscalls ) {
  fd_sandbox_private_unthreaded( uid, gid );
  fd_sandbox_private_threaded( close_fd_start, allow_syscalls_sz, allow_syscalls );
}

void
fd_sandbox_private_no_seccomp( uint close_fd_start ) {
  struct rlimit limit = { .rlim_cur = 0, .rlim_max = 0 };
  FD_TESTV( 0 == setrlimit( RLIMIT_NOFILE, &limit ));

  close_fds( close_fd_start );
}
