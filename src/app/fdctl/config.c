#include "fdctl.h"

#include "../../util/net/fd_eth.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if.h>
#include <arpa/inet.h>

FD_IMPORT_CSTR( default_config, "src/app/fdctl/config/default.toml" );

ulong
workspace_bytes( config_t * const config ) {
  ulong workspace_bytes = 0;
  if( FD_LIKELY( !strcmp( config->shmem.workspace_page_size, "gigantic" ) ) )
    workspace_bytes = config->shmem.workspace_page_count * 1024 * 1024 * 1024;
  else if( FD_LIKELY( !strcmp( config->shmem.workspace_page_size, "huge" ) ) )
    workspace_bytes = config->shmem.workspace_page_count * 2 * 1024 * 1024;
  else
    FD_LOG_ERR(( "invalid workspace_page_size: `%s`", config->shmem.workspace_page_size ));
  return workspace_bytes;
}

static char *
default_user( void ) {
  char * name = getenv( "SUDO_USER" );
  if( FD_UNLIKELY( name ) ) return name;

  name = getenv( "LOGNAME" );
  if( FD_LIKELY( name ) ) return name;

  name = getlogin();
  if( FD_UNLIKELY( !name ) ) FD_LOG_ERR(( "getlogin failed (%i-%s)", errno, strerror( errno ) ));
  return name;
}

static void parse_key_value( config_t *   config,
                             const char * section,
                             const char * key,
                             char * value ) {
#define ENTRY_STR(edot, esection, ekey) do {                                         \
    if( FD_UNLIKELY( !strcmp( section, #esection ) && !strcmp( key, #ekey ) ) ) {    \
      ulong len = strlen( value );                                                   \
      if( FD_UNLIKELY( len < 2 || value[ 0 ] != '"' || value[ len - 1 ] != '"' ) ) { \
        FD_LOG_ERR(( "invalid value for %s.%s: `%s`", section, key, value ));        \
        return;                                                                      \
      }                                                                              \
      if( FD_UNLIKELY( len >= sizeof( config->esection edot ekey ) + 2 ) )           \
        FD_LOG_ERR(( "value for %s.%s is too long: `%s`", section, key, value ));    \
      strncpy( config->esection edot ekey, value + 1, len - 2 );                     \
      config->esection edot ekey[ len - 2 ] = '\0';                                  \
      return;                                                                        \
    }                                                                                \
  } while( 0 )

#define ENTRY_VSTR(edot, esection, ekey) do {                                                        \
    if( FD_UNLIKELY( !strcmp( section, #esection ) && !strcmp( key, #ekey ) ) ) {                    \
      ulong len = strlen( value );                                                                   \
      if( FD_UNLIKELY( len < 2 || value[ 0 ] != '"' || value[ len - 1 ] != '"' ) ) {                 \
        FD_LOG_ERR(( "invalid value for %s.%s: `%s`", section, key, value ));                        \
        return;                                                                                      \
      }                                                                                              \
      if( FD_UNLIKELY( len >= sizeof( config->esection edot ekey[ 0 ] ) + 2 ) )                      \
        FD_LOG_ERR(( "value for %s.%s is too long: `%s`", section, key, value ));                    \
      if( FD_UNLIKELY( config->esection edot ekey##_cnt >= sizeof( config->esection edot ekey) ) )   \
        FD_LOG_ERR(( "too many values for %s.%s: `%s`", section, key, value ));                      \
      strncpy( config->esection edot ekey[ config->esection edot ekey##_cnt ], value + 1, len - 2 ); \
      config->esection edot ekey[ config->esection edot ekey##_cnt ][ len - 2 ] = '\0';              \
      config->esection edot ekey##_cnt++;                                                            \
      return;                                                                                        \
    }                                                                                                \
  } while( 0 )

#define ENTRY_UINT(edot, esection, ekey) do {                                     \
    if( FD_UNLIKELY( !strcmp( section, #esection ) && !strcmp( key, #ekey ) ) ) { \
      if( FD_UNLIKELY( strlen( value ) < 1 ) ) {                                  \
        FD_LOG_ERR(( "invalid value for %s.%s: `%s`", section, key, value ));     \
        return;                                                                   \
      }                                                                           \
      char * src = value;                                                         \
      char * dst = value;                                                         \
      while( *src ) {                                                             \
        if( *src != '_' ) *dst++ = *src;                                          \
        src++;                                                                    \
      }                                                                           \
      *dst = '\0';                                                                \
      char * endptr;                                                              \
      unsigned long int result = strtoul( value, &endptr, 10 );                   \
      if( FD_UNLIKELY( *endptr != '\0' || result > UINT_MAX ) ) {                 \
        FD_LOG_ERR(( "invalid value for %s.%s: `%s`", section, key, value ));     \
        return;                                                                   \
      }                                                                           \
      config->esection edot ekey = (uint)result;                                  \
      return;                                                                     \
    }                                                                             \
  } while( 0 )

#define ENTRY_VUINT(edot, esection, ekey) do {                                       \
    if( FD_UNLIKELY( !strcmp( section, #esection ) && !strcmp( key, #ekey ) ) ) {    \
      if( FD_UNLIKELY( strlen( value ) < 1 ) ) {                                     \
        FD_LOG_ERR(( "invalid value for %s.%s: `%s`", section, key, value ));        \
        return;                                                                      \
      }                                                                              \
      char * src = value;                                                            \
      char * dst = value;                                                            \
      while( *src ) {                                                                \
        if( *src != '_' ) *dst++ = *src;                                             \
        src++;                                                                       \
      }                                                                              \
      *dst = '\0';                                                                   \
      char * endptr;                                                                 \
      unsigned long int result = strtoul( value, &endptr, 10 );                      \
      if( FD_UNLIKELY( *endptr != '\0' || result > UINT_MAX ) ) {                    \
        FD_LOG_ERR(( "invalid value for %s.%s: `%s`", section, key, value ));        \
        return;                                                                      \
      }                                                                              \
      config->esection edot ekey[ config->esection edot ekey##_cnt ] = (uint)result; \
      config->esection edot ekey##_cnt++;                                            \
    }                                                                                \
  } while( 0 )

#define ENTRY_USHORT(edot, esection, ekey) do {                                   \
    if( FD_UNLIKELY( !strcmp( section, #esection ) && !strcmp( key, #ekey ) ) ) { \
      if( FD_UNLIKELY( strlen( value ) < 1 ) ) {                                  \
        FD_LOG_ERR(( "invalid value for %s.%s: `%s`", section, key, value ));     \
        return;                                                                   \
      }                                                                           \
      char * endptr;                                                              \
      unsigned long int result = strtoul( value, &endptr, 10 );                   \
      if( FD_UNLIKELY( *endptr != '\0' || result > USHORT_MAX ) ) {               \
        FD_LOG_ERR(( "invalid value for %s.%s: `%s`", section, key, value ));     \
        return;                                                                   \
      }                                                                           \
      config->esection edot ekey = (ushort)result;                                \
      return;                                                                     \
    }                                                                             \
  } while( 0 )

#define ENTRY_BOOL(edot, esection, ekey) do {                                     \
    if( FD_UNLIKELY( !strcmp( section, #esection ) && !strcmp( key, #ekey ) ) ) { \
      if( FD_LIKELY( !strcmp( value, "true" ) ) )                                 \
        config->esection edot ekey = 1;                                           \
      else if( FD_LIKELY( !strcmp( value, "false" ) ) )                           \
        config->esection edot ekey = 0;                                           \
      else                                                                        \
        FD_LOG_ERR(( "invalid value for %s.%s: `%s`", section, key, value ));     \
      return;                                                                     \
    }                                                                             \
  } while( 0 )

  ENTRY_STR   ( , ,                     name                                                      );
  ENTRY_STR   ( , ,                     user                                                      );
  ENTRY_STR   ( , ,                     scratch_directory                                         );
  ENTRY_STR   ( , ,                     dynamic_port_range                                        );

  ENTRY_STR   ( ., ledger,              path                                                      );
  ENTRY_STR   ( ., ledger,              accounts_path                                             );
  ENTRY_UINT  ( ., ledger,              limit_size                                                );
  ENTRY_BOOL  ( ., ledger,              bigtable_storage                                          );
  ENTRY_VSTR  ( ., ledger,              account_indexes                                           );
  ENTRY_VSTR  ( ., ledger,              account_index_exclude_keys                                );

  ENTRY_VSTR  ( ., gossip,              entrypoints                                               );
  ENTRY_BOOL  ( ., gossip,              port_check                                                );
  ENTRY_USHORT( ., gossip,              port                                                      );
  ENTRY_STR   ( ., gossip,              host                                                      );

  ENTRY_STR   ( ., consensus,           identity_path                                             );
  ENTRY_STR   ( ., consensus,           vote_account_path                                         );
  ENTRY_BOOL  ( ., consensus,           snapshot_fetch                                            );
  ENTRY_BOOL  ( ., consensus,           genesis_fetch                                             );
  ENTRY_BOOL  ( ., consensus,           poh_speed_test                                            );
  ENTRY_STR   ( ., consensus,           expected_genesis_hash                                     );
  ENTRY_UINT  ( ., consensus,           wait_for_supermajority_at_slot                            );
  ENTRY_STR   ( ., consensus,           expected_bank_hash                                        );
  ENTRY_USHORT( ., consensus,           expected_shred_version                                    );
  ENTRY_BOOL  ( ., consensus,           wait_for_vote_to_start_leader                             );
  ENTRY_VUINT ( ., consensus,           hard_fork_at_slots                                        );
  ENTRY_VSTR  ( ., consensus,           known_validators                                          );

  ENTRY_USHORT( ., rpc,                 port                                                      );
  ENTRY_BOOL  ( ., rpc,                 full_api                                                  );
  ENTRY_BOOL  ( ., rpc,                 private                                                   );
  ENTRY_BOOL  ( ., rpc,                 transaction_history                                       );
  ENTRY_BOOL  ( ., rpc,                 extended_tx_metadata_storage                              );
  ENTRY_BOOL  ( ., rpc,                 only_known                                                );
  ENTRY_BOOL  ( ., rpc,                 pubsub_enable_block_subscription                          );
  ENTRY_BOOL  ( ., rpc,                 pubsub_enable_vote_subscription                           );
  ENTRY_BOOL  ( ., rpc,                 incremental_snapshots                                     );

  ENTRY_STR   ( ., layout,              affinity                                                  );
  ENTRY_UINT  ( ., layout,              verify_tile_count                                         );

  ENTRY_STR   ( ., shmem,               gigantic_page_mount_path                                  );
  ENTRY_STR   ( ., shmem,               huge_page_mount_path                                      );
  ENTRY_UINT  ( ., shmem,               min_kernel_gigantic_pages                                 );
  ENTRY_UINT  ( ., shmem,               min_kernel_huge_pages                                     );
  ENTRY_STR   ( ., shmem,               workspace_page_size                                       );
  ENTRY_UINT  ( ., shmem,               workspace_page_count                                      );

  ENTRY_BOOL  ( ., development,         sandbox                                                   );
  ENTRY_BOOL  ( ., development,         sudo                                                      );

  ENTRY_BOOL  ( ., development.netns,   enabled                                                   );
  ENTRY_STR   ( ., development.netns,   interface0                                                );
  ENTRY_STR   ( ., development.netns,   interface0_mac                                            );
  ENTRY_STR   ( ., development.netns,   interface0_addr                                           );
  ENTRY_STR   ( ., development.netns,   interface1                                                );
  ENTRY_STR   ( ., development.netns,   interface1_mac                                            );
  ENTRY_STR   ( ., development.netns,   interface1_addr                                           );

  ENTRY_STR   ( ., tiles.quic,          interface                                                 );
  ENTRY_USHORT( ., tiles.quic,          listen_port                                               );
  ENTRY_UINT  ( ., tiles.quic,          max_concurrent_connections                                );
  ENTRY_UINT  ( ., tiles.quic,          max_concurrent_connection_ids_per_connection              );
  ENTRY_UINT  ( ., tiles.quic,          max_concurrent_streams_per_connection                     );
  ENTRY_UINT  ( ., tiles.quic,          max_concurrent_handshakes                                 );
  ENTRY_UINT  ( ., tiles.quic,          max_inflight_quic_packets                                 );
  ENTRY_UINT  ( ., tiles.quic,          tx_buf_size                                               );
  ENTRY_STR   ( ., tiles.quic,          xdp_mode                                                  );
  ENTRY_UINT  ( ., tiles.quic,          xdp_rx_queue_size                                         );
  ENTRY_UINT  ( ., tiles.quic,          xdp_tx_queue_size                                         );
  ENTRY_UINT  ( ., tiles.quic,          xdp_aio_depth                                             );

  ENTRY_UINT  ( ., tiles.verify,        receive_buffer_size                                       );
  ENTRY_UINT  ( ., tiles.verify,        mtu                                                       );

  ENTRY_UINT  ( ., tiles.pack,          max_pending_transactions                                  );
  ENTRY_UINT  ( ., tiles.pack,          compute_unit_estimator_table_size                         );
  ENTRY_UINT  ( ., tiles.pack,          compute_unit_estimator_ema_history                        );
  ENTRY_UINT  ( ., tiles.pack,          compute_unit_estimator_ema_default                        );
  ENTRY_UINT  ( ., tiles.pack,          solana_labs_bank_thread_count                             );
  ENTRY_UINT  ( ., tiles.pack,          solana_labs_bank_thread_compute_units_executed_per_second );

  ENTRY_UINT  ( ., tiles.dedup,         signature_cache_size                                      );
}

void
replace( char *       in,
         const char * pat,
         const char * sub ) {
  char * replace = strstr( in, pat );
  if( FD_LIKELY( replace ) ) {
    ulong pat_len = strlen( pat );
    ulong sub_len = strlen( sub );
    ulong in_len  = strlen( in );
    if( FD_UNLIKELY( pat_len > in_len ) ) return;

    ulong total_len = in_len - pat_len + sub_len;
    if( FD_UNLIKELY( total_len >= PATH_MAX ) )
      FD_LOG_ERR(( "configuration scratch directory path too long: `%s`", in ));

    uchar after[PATH_MAX];
    fd_memcpy( after, replace + pat_len, strlen( replace + pat_len ) );
    fd_memcpy( replace, sub, sub_len );
    ulong after_len = strlen( ( const char * ) after );
    fd_memcpy( replace + sub_len, after, after_len );
    in[ total_len ] = '\0';
  }
}



static void
config_parse_array( config_t * config,
                    char * section,
                    char * key,
                    int * in_array,
                    char * value ) {
  char * end = value + strlen( value ) - 1;
  while( FD_UNLIKELY( *end == ' ' ) ) end--;
  if( FD_LIKELY( *end == ']' ) ) {
    *end = '\0';
    *in_array = 0;
  }

  char * saveptr;
  char * token = strtok_r( value, ",", &saveptr );
  while( token ) {
    while( FD_UNLIKELY( *token == ' ' ) ) token++;
    char * end = token + strlen( token ) - 1;
    while( FD_UNLIKELY( *end == ' ' ) ) end--;
    *(end+1) = '\0';
    if( FD_LIKELY( end > token ) ) parse_key_value( config, section, key, token );
    token = strtok_r( NULL, ",", &saveptr );
  }
}

static void
config_parse_line( uint       lineno,
                   char *     line,
                   char *     section,
                   int *      in_array,
                   char *     key,
                   config_t * out ) {
  while( FD_LIKELY( *line == ' ' ) ) line++;
  if( FD_UNLIKELY( *line == '#' || *line == '\0' || *line == '\n' ) ) return;

  if( FD_UNLIKELY( *in_array ) ) {
    config_parse_array( out, section, key, in_array, line );
    return;
  }

  if( FD_UNLIKELY( *line == '[' ) ) {
    char * end = strchr( line, ']' );
    if( FD_UNLIKELY( !end ) ) FD_LOG_ERR(( "invalid line %u: no closing bracket `%s`", lineno, line ));
    if( FD_UNLIKELY( *(end+1) != '\0' ) ) FD_LOG_ERR(( "invalid line %u: no newline after closing bracket `%s`", lineno, line ));
    *end = '\0';
    strcpy( section, line + 1 );
    return;
  }

  char * equals = strchr( line, '=' );
  if( FD_UNLIKELY( !equals ) ) FD_LOG_ERR(( "invalid line %u: no equal character `%s`", lineno, line ));

  char * value = equals + 1;
  while( FD_LIKELY( *value == ' ' ) ) value++;
  while ( FD_UNLIKELY( equals > line && *(equals - 1) == ' ' ) ) equals--;

  *equals = '\0';
  strcpy( key, line );

  if( FD_UNLIKELY( *value == '[' ) ) {
    *in_array = 1;
    config_parse_array( out, section, key, in_array, value );
  } else {
    parse_key_value( out, section, key, value );
  }
}

static void
config_parse1( const char * config,
               config_t *   out ) {
  char section[ 4096 ];
  char key[ 4096 ];
  uint lineno = 0;
  int in_array = 0;
  const char * line = config;
  while( line ) {
    lineno++;
    char * next_line = strchr( line, '\n' );

    ulong n = next_line ? (ulong)(next_line - line) : strlen( line );
    if( n >= 4096 ) FD_LOG_ERR(( "line %u too long `%s`", lineno, line ));

    char line_copy[ 4096 ];
    strncpy( line_copy, line, sizeof( line_copy ) - 1 ); // -1 to silence linter
    line_copy[ n ] = '\0';

    config_parse_line( lineno, line_copy, section, &in_array, key, out );

    if( FD_LIKELY( next_line ) ) next_line++;
    line = next_line;
  }
}

static void
config_parse_file( const char * path,
                   config_t *   out ) {
  FILE * fp = fopen( path, "r" );
  if( FD_UNLIKELY( !fp ) ) FD_LOG_ERR(( "could not open configuration file `%s`: (%d-%s)", path, errno, strerror( errno ) ));

  uint lineno = 0;
  char line[ 4096 ];
  char key[ 4096 ];
  int in_array = 0;
  char section[ 4096 ];
  while( FD_LIKELY( fgets( line, 4096, fp ) ) ) {
    lineno++;
    if( FD_UNLIKELY( strlen( line ) == 4095 ) ) FD_LOG_ERR(( "line too long in `%s`", path ));
    config_parse_line( lineno, line, section, &in_array, key, out );
  }
  if( FD_UNLIKELY( ferror( fp ) ) )
    FD_LOG_ERR(( "error reading `%s` (%i-%s)", path, errno, strerror( errno ) ));
  if( FD_LIKELY( fclose( fp ) ) )
    FD_LOG_ERR(( "error closing `%s` (%i-%s)", path, errno, strerror( errno ) ));
}

static uint
listen_address( const char * interface ) {
  int fd = socket( AF_INET, SOCK_DGRAM, 0 );
  struct ifreq ifr = {0};
  ifr.ifr_addr.sa_family = AF_INET;
  strncpy( ifr.ifr_name, interface, IF_NAMESIZE );
  if( FD_UNLIKELY( ioctl( fd, SIOCGIFADDR, &ifr ) ) )
    FD_LOG_ERR(( "could not get IP address of interface `%s`: (%d-%s)", interface, errno, strerror( errno ) ));
  if( FD_UNLIKELY( close(fd) ) )
    FD_LOG_ERR(( "could not close socket (%d-%s)", errno, strerror( errno ) ));
  return ((struct sockaddr_in *)fd_type_pun( &ifr.ifr_addr ))->sin_addr.s_addr;
}

static void
mac_address( const char * interface,
             uchar *      mac ) {
  int fd = socket( AF_INET, SOCK_DGRAM, IPPROTO_IP );
  struct ifreq ifr;
  ifr.ifr_addr.sa_family = AF_INET;
  strncpy( ifr.ifr_name, interface, IF_NAMESIZE );
  if( FD_UNLIKELY( ioctl( fd, SIOCGIFHWADDR, &ifr ) ) )
    FD_LOG_ERR(( "could not get MAC address of interface `%s`: (%d-%s)", interface, errno, strerror( errno ) ));
  if( FD_UNLIKELY( close(fd) ) )
    FD_LOG_ERR(( "could not close socket (%d-%s)", errno, strerror( errno ) ));
  fd_memcpy( mac, ifr.ifr_hwaddr.sa_data, 6 );
}

config_t
config_parse( int *    pargc,
              char *** pargv ) {
  config_t result = {0};
  config_parse1( default_config, &result );

  const char * user_config = fd_env_strip_cmdline_cstr(
      pargc,
      pargv,
      "--config",
      "FIREDANCER_CONFIG_FILE",
      NULL );

  if( FD_LIKELY( user_config ) ) {
    config_parse_file( user_config, &result );
  }

  if( FD_UNLIKELY( !strcmp( result.user, "" ) ) ) {
    const char * user = default_user();
    if( FD_UNLIKELY( strlen( user ) >= sizeof( result.user ) ) )
      FD_LOG_ERR(( "user name `%s` is too long", user ));
    strncpy( result.user, user, 256 );
  }

  if( FD_UNLIKELY( !strcmp( result.tiles.quic.interface, "" ) ) ) {
    int ifindex = internet_routing_interface();
    if( FD_UNLIKELY( ifindex == -1 ) )
      FD_LOG_ERR(( "no network device found which routes to 8.8.8.8. If no network "
                   "interface is specified in the configuration file, Firedancer "
                   "will use the first network interface found which routes to "
                   "8.8.8.8. You can see what this is by running `ip route get 8.8.8.8` "
                   "You can fix this error by specifying a network interface to bind to in "
                   "your configuration file under [tiles.quic.interface]" ));

    if( FD_UNLIKELY( !if_indextoname( (uint)ifindex, result.tiles.quic.interface ) ) )
      FD_LOG_ERR(( "could not get name of interface with index %u", ifindex ));
  }

  if( FD_UNLIKELY( result.development.netns.enabled ) ) {
    if( FD_UNLIKELY( strcmp( result.development.netns.interface0, result.tiles.quic.interface ) ) )
      FD_LOG_ERR(( "netns interface and quic interface are different. If you are using the "
                   "[development.netns] functionality to run Firedancer in a network namespace "
                   "for development, the configuration file must specify that "
                   "[development.netns.interface0] is the same as [tiles.quic.interface]" ));

    fd_cstr_to_ip4_addr( result.development.netns.interface0_addr, &result.tiles.quic.ip_addr );
    fd_cstr_to_mac_addr( result.development.netns.interface0_mac, result.tiles.quic.mac_addr );
  } else {
    if( FD_UNLIKELY( !if_nametoindex( result.tiles.quic.interface ) ) )
      FD_LOG_ERR(( "configuration specifies network interface `%s` which does not exist", result.tiles.quic.interface ));
    result.tiles.quic.ip_addr = listen_address( result.tiles.quic.interface );
    mac_address( result.tiles.quic.interface, result.tiles.quic.mac_addr );
  }

  errno = 0;
  struct passwd * passwd = getpwnam( result.user );
  if( FD_UNLIKELY( !passwd && !errno ))
    FD_LOG_ERR(( "configuration file wants firedancer to run as user `%s` but it does not exist", result.user ));
  else if( FD_UNLIKELY( !passwd ) )
    FD_LOG_ERR(( "getpwnam failed (%i-%s)", errno, strerror( errno ) ) );

  result.uid = passwd->pw_uid;
  result.gid = passwd->pw_uid;

  if( result.uid == 0 || result.gid == 0 )
    FD_LOG_ERR(( "firedancer cannot run as root. please specify a non-root user in the configuration file" ));

  if( FD_UNLIKELY( getuid() != 0 && result.uid != getuid() ) )
    FD_LOG_ERR(( "running as uid %i, but config specifies uid %i", getuid(), result.uid ));
  if( FD_UNLIKELY( getgid() != 0 && result.gid != getgid() ) )
    FD_LOG_ERR(( "running as gid %i, but config specifies gid %i", getuid(), result.uid ));

  replace( result.scratch_directory, "{user}", result.user );
  replace( result.scratch_directory, "{name}", result.name );

  if( FD_UNLIKELY( strcmp( result.ledger.path, "" ) ) ) {
    replace( result.ledger.path, "{user}", result.user );
    replace( result.ledger.path, "{name}", result.name );
  } else {
    snprintf1( result.ledger.path, sizeof(result.ledger.path), "%s/ledger", result.scratch_directory );
  }

  if( FD_UNLIKELY( !strcmp( result.consensus.identity_path, "" ) ) ) {
    snprintf1( result.consensus.identity_path,
               sizeof(result.consensus.identity_path),
               "%s/identity.json",
               result.scratch_directory );
  } else {
    replace( result.consensus.identity_path, "{user}", result.user );
    replace( result.consensus.identity_path, "{name}", result.name );
  }

  replace( result.consensus.vote_account_path, "{user}", result.user );
  replace( result.consensus.vote_account_path, "{name}", result.name );

  result.is_live_cluster = 0;
  for( ulong i=0; i<result.gossip.entrypoints_cnt; i++ ) {
    if( strstr( result.gossip.entrypoints[ i ], "solana.com" ) ||
        strstr( result.gossip.entrypoints[ i ], "pyth.network" ) ) {
      result.is_live_cluster = 1;
      break;
    }
  }

  const char * live_genesis_hashes[ 6 ] = {
    "EtWTRABZaYq6iMfeYKouRu166VU2xqa1wcaWoxPkrZBG", // devnet
    "4uhcVJyU9pJkvQyS88uRDiswHXSCkY3zQawwpjk2NsNY", // testnet
    "5eykt4UsFv8P8NJdTREpY1vzqKqZKvdpKuc147dw2N9d", // mainnet
    "EkCkB7RWVrgkcpariRpd3pjf7GwiCMZaMHKUpB5Na1Ve", // pythtest
    "GLKkBUr6r72nBtGrtBPJLRqtsh8wXZanX4xfnqKnWwKq", // pythnet
    NULL,
  };

  for( ulong i=0; live_genesis_hashes[ i ]; i++ ) {
    if( !strcmp( result.consensus.expected_genesis_hash, live_genesis_hashes[ i ] ) ) {
      result.is_live_cluster = 1;
      break;
    }
  }

  if( FD_UNLIKELY( result.is_live_cluster ) )
    FD_LOG_EMERG(( "Attempted to start against a live cluster. Firedancer is not "
                   "ready for production deployment, has not been tested, and is "
                   "missing consensus critical functionality. Joining a live Solana "
                   "cluster may destabilize the network. Please do not attempt." ));

  if( FD_LIKELY( result.is_live_cluster) ) {
    if( FD_UNLIKELY( result.development.sudo ) )
      FD_LOG_ERR(( "trying to join a live cluster, but configuration specified [development.sudo] which is a development only feature" ));
    if( FD_UNLIKELY( !result.development.sandbox ) )
      FD_LOG_ERR(( "trying to join a live cluster, but configuration disables the sandbox which is a a development only feature" ));
    if( FD_UNLIKELY( result.development.netns.enabled ) )
      FD_LOG_ERR(( "trying to join a live cluster, but configuration enables [development.netns] which is a development only feature" ));
  }

  return result;
}

void
dump_vars( config_t * const config,
           const char *     pod,
           const char *     main_cnc ) {
  char path[ PATH_MAX ];
  snprintf1( path, PATH_MAX, "%s/config.cfg", config->scratch_directory );

  mkdir_all( config->scratch_directory, config->uid, config->gid );

  /* switch to non-root uid/gid for file creation. permissions checks still done as root. */
  gid_t gid = getgid();
  uid_t uid = getuid();
  if( FD_LIKELY( gid == 0 && setegid( config->gid ) ) )
    FD_LOG_ERR(( "setegid() failed (%i-%s)", errno, strerror( errno ) ));
  if( FD_LIKELY( uid == 0 && seteuid( config->uid ) ) )
    FD_LOG_ERR(( "seteuid() failed (%i-%s)", errno, strerror( errno ) ));

  FILE * fp = fopen( path, "w" );
  if( FD_UNLIKELY( !fp ) ) FD_LOG_ERR(( "failed to open `%s` for writing: %s", path, strerror( errno ) ));

  int err = fprintf( fp,
                     "#!/bin/bash\n"
                     "# AUTOGENERATED\n"
                     "WKSP=%s.wksp\n"
                     "AFFINITY=%s\n"
                     "APP=%s\n"
                     "POD=%s\n"
                     "MAIN_CNC=%s.wksp:%s\n"
                     "IFACE=%s\n",
                     config->name,
                     config->layout.affinity,
                     config->name,
                     pod,
                     config->name,
                     main_cnc,
                     config->tiles.quic.interface );
  if( FD_UNLIKELY( err < 0 ) ) FD_LOG_ERR(( "fprintf failed (%i-%s)", errno, strerror( errno ) ));
  if( FD_UNLIKELY( fclose( fp ) ) ) FD_LOG_ERR(( "fclose failed `%s` (%d-%s)", path, errno, strerror( errno ) ));

  if( FD_UNLIKELY( seteuid( uid ) ) ) FD_LOG_ERR(( "seteuid() failed (%i-%s)", errno, strerror( errno ) ));
  if( FD_UNLIKELY( setegid( gid ) ) ) FD_LOG_ERR(( "setegid() failed (%i-%s)", errno, strerror( errno ) ));
}

const char *
load_var_pod( config_t * const config,
              char * name,
              char line[4096] ) {
  char path[ PATH_MAX ];
  snprintf1( path, sizeof(path), "%s/config.cfg", config->scratch_directory );

  FILE * fp = fopen( path, "r" );
  if( FD_UNLIKELY( !fp ) ) FD_LOG_ERR(( "failed to open %s: (%d-%s)", path, errno, strerror( errno ) ));

  ulong i = 0;
  while( FD_LIKELY( fgets( line, 4096, fp ) ) ) {
    if( FD_UNLIKELY( strlen( line ) == 4095 ) ) FD_LOG_ERR(( "line too long in `%s`", path ));
    if( FD_UNLIKELY( i++<2 ) ) continue;

    char * eq = strchr( line, '=' );
    if( FD_UNLIKELY( !eq ) ) FD_LOG_ERR(( "malformed config.cfg (expected `=`): `%s`", line ));

    *eq++ = '\0';
    eq[ strlen( eq ) - 1 ] = '\0';
    if( FD_UNLIKELY( !strcmp( line, name ) ) ) {
      if( FD_UNLIKELY( fclose( fp ) ) ) FD_LOG_ERR(( "close failed (%d-%s)", errno, strerror( errno ) ));
      return eq;
    }
  }

  FD_LOG_ERR(( "malformed config.cfg (expected to find `%s=`): `%s`", name, path ));
}
