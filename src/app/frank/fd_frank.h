#ifndef HEADER_fd_src_app_frank_fd_frank_h
#define HEADER_fd_src_app_frank_fd_frank_h

#include "../../disco/fd_disco.h"
#include "../../ballet/fd_ballet.h" /* FIXME: CONSIDER HAVING THIS IN DISCO_BASE */
#include "../../tango/xdp/fd_xsk.h"

/* FD_FRANK_CNC_DIAG_* are FD_CNC_DIAG_* style diagnostics and thus the
   same considerations apply.  Further they are harmonized with the
   standard FD_CNC_DIAG_*.  Specifically:

     IN_BACKP is same as standard IN_BACKP

     BACKP_CNT is same as standard BACKP_CNT

     {HA,SV}_FILT_{CNT,SZ} is frank specific and the number of times a
     transaction was dropped by a verify tile due to failing signature
     verification. */

#define FD_FRANK_CNC_DIAG_IN_BACKP    FD_CNC_DIAG_IN_BACKP  /* ==0 */
#define FD_FRANK_CNC_DIAG_BACKP_CNT   FD_CNC_DIAG_BACKP_CNT /* ==1 */
#define FD_FRANK_CNC_DIAG_HA_FILT_CNT (2UL)                 /* updated by verify tile, frequently in ha situations, never o.w. */
#define FD_FRANK_CNC_DIAG_HA_FILT_SZ  (3UL)                 /* " */
#define FD_FRANK_CNC_DIAG_SV_FILT_CNT (4UL)                 /* ", ideally never */
#define FD_FRANK_CNC_DIAG_SV_FILT_SZ  (5UL)                 /* " */

typedef struct {
   char          name[ 32 ];
   ulong         tile_idx;
   ulong         idx;
   char const *  pod_gaddr;
   uchar const * pod;
   fd_xsk_t    * xsk;
   uint          close_fd_start;
   ushort        allow_syscalls_sz;
   long *        allow_syscalls;
} fd_frank_args_t;

typedef struct {
   char * name;
   void (*init)( fd_frank_args_t * args );
   void (*run )( fd_frank_args_t * args );
} fd_frank_task_t;

extern fd_frank_task_t verify;
extern fd_frank_task_t dedup;
extern fd_frank_task_t quic;
extern fd_frank_task_t pack;

FD_PROTOTYPES_BEGIN

void
fd_frank_mon( const uchar * pod,
              long          dt_min,
              long          dt_max,
              long          duration,
              uint          seed );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_app_frank_fd_frank_h */

