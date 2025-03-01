#define _GNU_SOURCE
#include "configure.h"

#include "../../../tango/xdp/fd_xdp_redirect_user.h"

#include <unistd.h>
#include <sys/stat.h>
#include <linux/capability.h>
#include <linux/if_link.h>

#define NAME "xdp"

static void
init_perm( security_t *     security,
           config_t * const config ) {
  if( FD_UNLIKELY( config->development.netns.enabled ) )
    check_cap( security, NAME, CAP_SYS_ADMIN, "enter a network namespace" );
  else {
    check_cap( security, NAME, CAP_SYS_ADMIN, "create a BPF map with `bpf_map_create`" );
    check_cap( security, NAME, CAP_NET_ADMIN, "create an XSK map with `bpf_map_create`" );
  }
}

/* fd_xdp_redirect_prog is eBPF ELF object containing the XDP program.
   It is embedded into this program. Build with `make ebpf-bin`. */
FD_IMPORT_BINARY( fd_xdp_redirect_prog, "src/tango/xdp/fd_xdp_redirect_prog.o" );

static void
init( config_t * const config ) {
  enter_network_namespace( config );

  uint mode = 0;
  if(      FD_LIKELY( !strcmp( config->tiles.quic.xdp_mode, "skb" ) ) ) mode = XDP_FLAGS_SKB_MODE;
  else if( FD_LIKELY( !strcmp( config->tiles.quic.xdp_mode, "drv" ) ) ) mode = XDP_FLAGS_DRV_MODE;
  else if( FD_LIKELY( !strcmp( config->tiles.quic.xdp_mode, "hw"  ) ) ) mode = XDP_FLAGS_HW_MODE;
  else FD_LOG_ERR(( "unknown XDP mode `%s`", config->tiles.quic.xdp_mode ));

  if( FD_UNLIKELY( fd_xdp_init( config->name,
                                0750,
                                (int)config->uid,
                                (int)config->uid ) ) )
    FD_LOG_ERR(( "fd_xdp_init failed" ));
  if( FD_UNLIKELY( fd_xdp_hook_iface( config->name,
                                      config->tiles.quic.interface,
                                      mode,
                                      fd_xdp_redirect_prog,
                                      fd_xdp_redirect_prog_sz ) ) )
    FD_LOG_ERR(( "fd_xdp_hook_iface failed" ));
  if( FD_UNLIKELY( fd_xdp_listen_udp_port( config->name,
                                           config->tiles.quic.ip_addr,
                                           config->tiles.quic.listen_port, 1 ) ) )
    FD_LOG_ERR(( "fd_xdp_listen_udp_port failed" ));
}

static void
fini( config_t * const config ) {
  if( FD_UNLIKELY( fd_xdp_fini( config->name ) ) )
    FD_LOG_ERR(( "fd_xdp_fini failed" ));

  /* work around race condition, ugly hack due to kernel maybe removing
     some hooks in the background */
  nanosleep1( 1, 0 );

  char path[ PATH_MAX ];
  snprintf1( path, PATH_MAX, "/sys/fs/bpf/%s/%s", config->name, config->tiles.quic.interface );
  if( FD_UNLIKELY( rmdir( path ) && errno != ENOENT ) ) FD_LOG_ERR(( "rmdir failed (%i-%s)", errno, strerror( errno ) ));
  snprintf1( path, PATH_MAX, "/sys/fs/bpf/%s", config->name );
  if( FD_UNLIKELY( rmdir( path ) && errno != ENOENT ) ) FD_LOG_ERR(( "rmdir failed (%i-%s)", errno, strerror( errno ) ));
}

static configure_result_t
check( config_t * const config ) {
  char xdp_path[ PATH_MAX ];
  snprintf1( xdp_path, PATH_MAX, "/sys/fs/bpf/%s", config->name );

  struct stat st;
  int result = stat( xdp_path, &st );
  if( FD_UNLIKELY( result && errno == ENOENT ) ) NOT_CONFIGURED( "`%s` does not exist", xdp_path );
  else if( FD_UNLIKELY( result ) ) PARTIALLY_CONFIGURED( "`%s` cannot be statted (%i-%s)", xdp_path, errno, strerror( errno ) );

  CHECK( check_dir(  "/sys/fs/bpf", config->uid, config->uid, S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP ) );
  CHECK( check_dir(  xdp_path,      config->uid, config->uid, S_IFDIR | S_IRWXU | S_IRGRP | S_IXGRP ) );

  snprintf1( xdp_path, PATH_MAX, "/sys/fs/bpf/%s/udp_dsts", config->name );
  CHECK( check_file( xdp_path,      config->uid, config->uid, S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP ) );

  snprintf1( xdp_path, PATH_MAX, "/sys/fs/bpf/%s/%s/xdp_link", config->name, config->tiles.quic.interface );
  CHECK( check_file( xdp_path,      config->uid, config->uid, S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP ) );

  snprintf1( xdp_path, PATH_MAX, "/sys/fs/bpf/%s/%s/xdp_prog", config->name, config->tiles.quic.interface );
  CHECK( check_file( xdp_path,      config->uid, config->uid, S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP ) );

  snprintf1( xdp_path, PATH_MAX, "/sys/fs/bpf/%s/%s/xsks", config->name, config->tiles.quic.interface );
  CHECK( check_file( xdp_path,      config->uid, config->uid, S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP ) );

  /* todo: step into these links and make sure the interior data is
           correct */
  CONFIGURE_OK();
}

configure_stage_t xdp = {
  .name            = NAME,
  .always_recreate = 0,
  .enabled         = NULL,
  .init_perm       = init_perm,
  .fini_perm       = NULL,
  .init            = init,
  .fini            = fini,
  .check           = check,
};

#undef NAME
