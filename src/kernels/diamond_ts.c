#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include "mpi.h"
#include "data_structures.h"

#define ST_BUSY (0)
#define ST_NOT_BUSY (1)

extern void sub_array_copy_tg(const FLOAT_PRECISION * restrict src_buf, FLOAT_PRECISION * restrict dst_buf, int *src_size, int *dst_size, int *cpy_size, int *src_offs, int *dst_offs, int);

typedef struct{
volatile  int *t_pos;
  int *state;
} Diam_Sched_State;
volatile int *avail_list;
volatile unsigned long int head, tail;

int diam_width;
Diam_Sched_State st;
int y_len_l, y_len_r;
int t_len;
int mpi_size;
FLOAT_PRECISION *send_buf_l, *recv_buf_l, *send_buf_r, *recv_buf_r;
MPI_Request wait_req_send_l[2], wait_req_recv_l[2], wait_req_send_r[2], wait_req_recv_r[2];



extern struct Kernel KernelList[];

void intra_diamond_trapzd_comp(Parameters *p, int yb, int ye){
  int t;
  // use all the threads in the initialization of the time stepper
  int swp_tgs = p->stencil_ctx.thread_group_size;
  p->stencil_ctx.thread_group_size = p->num_threads;
  for(t=0; t<p->t_dim+1; t++){
    if(t%2 == 0){
      KernelList[p->target_kernel].spt_blk_func(p->ldomain_shape, NHALO, yb, NHALO, p->lstencil_shape[0]+NHALO, ye, p->ldomain_shape[2]-NHALO, p->coef, p->U1, p->U2, p->U3, p->stencil_ctx);
//      if(p->main_source_state[state] == 1) U1(p->lsource_pt[0],p->lsource_pt[1],p->lsource_pt[2]) += p->source[it+t];
    }else{
      KernelList[p->target_kernel].spt_blk_func(p->ldomain_shape, NHALO, yb, NHALO, p->lstencil_shape[0]+NHALO, ye, p->ldomain_shape[2]-NHALO, p->coef, p->U2, p->U1, p->U3, p->stencil_ctx);
//      if(p->main_source_state[state] == 1) U2(p->lsource_pt[0],p->lsource_pt[1],p->lsource_pt[2]) += p->source[it+t];
    }
    yb += NHALO;
    ye -= NHALO;
  }
  p->stencil_ctx.thread_group_size = swp_tgs;

}

void intra_diamond_inv_trapzd_comp(Parameters *p, int it, int yb, int ye){
  int t;
  int t_dim = p->t_dim;
  // use all the threads in the initialization of the time stepper
  int swp_tgs = p->stencil_ctx.thread_group_size;
  p->stencil_ctx.thread_group_size = p->num_threads;

  for(t=0; t<t_dim+1; t++){
//    printf("[%03d] inverted trapzd: state:%02d zb:%02d ze:%02d t:%03d\n", p->mpi_rank, state, zb, ze, t);
    if(it%2 == 0){
      KernelList[p->target_kernel].spt_blk_func(p->ldomain_shape, NHALO, yb, NHALO, p->lstencil_shape[0]+NHALO, ye, p->ldomain_shape[2]-NHALO, p->coef, p->U1, p->U2, p->U3, p->stencil_ctx);
//      if(p->main_source_state[state] == 1) U1(p->lsource_pt[0],p->lsource_pt[1],p->lsource_pt[2]) += p->source[it+t];
    }else{
      KernelList[p->target_kernel].spt_blk_func(p->ldomain_shape, NHALO, yb, NHALO, p->lstencil_shape[0]+NHALO, ye, p->ldomain_shape[2]-NHALO, p->coef, p->U2, p->U1, p->U3, p->stencil_ctx);
//      if(p->main_source_state[state] == 1) U2(p->lsource_pt[0],p->lsource_pt[1],p->lsource_pt[2]) += p->source[it+t];
    }
    yb -= NHALO;
    ye += NHALO;
    it++;
  }
  p->stencil_ctx.thread_group_size = swp_tgs;
}


void intra_diamond_comp(Parameters *p, int yb, int ye, int it, int b_inc, int e_inc){
  int t;

  for(t=0; t< (p->t_dim+1)*2-1; t++){
    if(it%2 == 0){
      KernelList[p->target_kernel].spt_blk_func(p->ldomain_shape, NHALO, yb, NHALO, p->lstencil_shape[0]+NHALO, ye, p->ldomain_shape[2]-NHALO, p->coef, p->U1, p->U2, p->U3, p->stencil_ctx);
    }else{
      KernelList[p->target_kernel].spt_blk_func(p->ldomain_shape, NHALO, yb, NHALO, p->lstencil_shape[0]+NHALO, ye, p->ldomain_shape[2]-NHALO, p->coef, p->U2, p->U1, p->U3, p->stencil_ctx);
    }

    if(t< p->t_dim){ // inverted trapezoid (or lower half of the diamond)
      yb -= b_inc;
      ye += e_inc;
    }else{ // trapezoid  (or upper half of the diamond)
      yb += b_inc;
      ye -= e_inc;
    }
    it++;
  }
}


void intra_diamond_1wf_comp(Parameters *p, int yb_r, int ye_r, int b_inc, int e_inc){
  int t, z, zb, ze;
  int tb = p->t_dim*2+1; //temporal block size
  int yb, ye;
//printf("HERE 1\n");

  double t1, t2, t3;
  int tid = 0;
#if defined(_OPENMP)
    tid = omp_get_thread_num();
#endif

  // wavefront prologue
  t1 = MPI_Wtime();

  yb = yb_r;
  ye = ye_r;
  zb = NHALO;
  for(t=0; t< tb-1; t++){
    //    for(z=NHALO; z< NHALO*(tb-t); z++) // NHALO*(tb-t) = NHALO + NHALO*(tb-t-1)
    {
      ze = NHALO*(tb-t);
      if(t%2 == 1){
        KernelList[p->target_kernel].spt_blk_func(p->ldomain_shape, NHALO, yb, zb, p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U1, p->U2, p->U3, p->stencil_ctx);
      }else{
        KernelList[p->target_kernel].spt_blk_func(p->ldomain_shape, NHALO, yb, zb, p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U2, p->U1, p->U3, p->stencil_ctx);
      }

      if(t< p->t_dim){ // inverted trapezoid (or lower half of the diamond)
        yb -= b_inc;
        ye += e_inc;
      }else{ // trapezoid  (or upper half of the diamond)
        yb += b_inc;
        ye -= e_inc;
      }

    }
  }


  t2 = MPI_Wtime();
  // main wavefront loop
  yb = yb_r;
  ye = ye_r;
  zb = NHALO+(tb-1)*NHALO;
  ze = p->ldomain_shape[2]-NHALO;

  // compute the multi-wavefront steps
  KernelList[p->target_kernel].swd_func(p->ldomain_shape, NHALO, yb, zb,
      p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U1, p->U2, p->U3, p->t_dim, b_inc, e_inc, p->stencil_ctx, 0);
  t3 = MPI_Wtime();


  // wavefront epilogue
  yb = yb_r - b_inc;
  ye = ye_r + e_inc;
  ze = p->ldomain_shape[2]-NHALO;
  for(t=1; t< tb; t++){
    //    for(z=p->ldomain_shape[2]-NHALO - t*NHALO; z<p->ldomain_shape[2]-NHALO; z++)
    {
      zb = p->ldomain_shape[2]-NHALO - t*NHALO;
      if(t%2 == 1){
        KernelList[p->target_kernel].spt_blk_func(p->ldomain_shape, NHALO, yb, zb, p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U1, p->U2, p->U3, p->stencil_ctx);
      }else{
        KernelList[p->target_kernel].spt_blk_func(p->ldomain_shape, NHALO, yb, zb, p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U2, p->U1, p->U3, p->stencil_ctx);
      }

      if((t)< p->t_dim){ // inverted trapezoid (or lower half of the diamond)
        yb -= b_inc;
        ye += e_inc;
      }else{ // trapezoid  (or upper half of the diamond)
        yb += b_inc;
        ye -= e_inc;
      }
    }
  }


  p->stencil_ctx.t_wf_prologue[tid] += t2-t1;
  p->stencil_ctx.t_wf_main[tid]     += t3-t2;
  p->stencil_ctx.t_wf_epilogue[tid] += MPI_Wtime() - t3;
}


void intra_diamond_all_mwf_comp(Parameters *p, int yb_r, int ye_r, int b_inc, int e_inc, int tid){
  int t, z, zb, ze, tnwf;
  int tb = p->t_dim*2+1; //temporal block size
  int yb, ye;
//printf("HERE 1\n");

  double t1, t2, t3;

  // wavefront prologue
  t1 = MPI_Wtime();

  yb = yb_r;
  ye = ye_r;
  zb = NHALO;
  for(t=0; t< tb-1; t++){
    //    for(z=NHALO; z< NHALO*(tb-t); z++) // NHALO*(tb-t) = NHALO + NHALO*(tb-t-1)
    {
      ze = NHALO*(tb-t);
      if(t%2 == 1){
        KernelList[p->target_kernel].stat_sched_func(p->ldomain_shape, NHALO, yb, zb, p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U1, p->U2, p->U3, p->stencil_ctx);
      }else{
        KernelList[p->target_kernel].stat_sched_func(p->ldomain_shape, NHALO, yb, zb, p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U2, p->U1, p->U3, p->stencil_ctx);
      }

      if(t< p->t_dim){ // inverted trapezoid (or lower half of the diamond)
        yb -= b_inc;
        ye += e_inc;
      }else{ // trapezoid  (or upper half of the diamond)
        yb += b_inc;
        ye -= e_inc;
      }

    }
  }


  t2 = MPI_Wtime();
  // main wavefront loop
  yb = yb_r;
  ye = ye_r;
  zb = NHALO+(tb-1)*NHALO;
  ze = p->ldomain_shape[2]-NHALO;
//  ze -= (ze-zb)%p->stencil_ctx.num_wf;

  // compute the multi-wavefront steps
//  KernelList[p->target_kernel].mwd_func(p->ldomain_shape, NHALO, yb, zb,
//      p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U1, p->U2, p->U3, p->t_dim, b_inc, e_inc, p->stencil_ctx);
  p->mwd_func(p->ldomain_shape, NHALO, yb, zb,
        p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U1, p->U2, p->U3, p->t_dim, b_inc, e_inc, p->stencil_ctx, tid);

//  tnwf = p->stencil_ctx.num_wf;
//  p->stencil_ctx.num_wf = 1;
//
  // compute the remainders of the multi-wavefront steps
//  zb = ze;
//  ze = p->ldomain_shape[2]-NHALO;
//  KernelList[p->target_kernel].swd_func(p->ldomain_shape, NHALO, yb, zb,
//      p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U1, p->U2, p->U3, p->t_dim, b_inc, e_inc, p->stencil_ctx);
//  p->stencil_ctx.num_wf = tnwf;
  t3 = MPI_Wtime();


  // wavefront epilogue
  yb = yb_r - b_inc;
  ye = ye_r + e_inc;
  ze = p->ldomain_shape[2]-NHALO;
  for(t=1; t< tb; t++){
    //    for(z=p->ldomain_shape[2]-NHALO - t*NHALO; z<p->ldomain_shape[2]-NHALO; z++)
    {
      zb = p->ldomain_shape[2]-NHALO - t*NHALO;
      if(t%2 == 1){
        KernelList[p->target_kernel].stat_sched_func(p->ldomain_shape, NHALO, yb, zb, p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U1, p->U2, p->U3, p->stencil_ctx);
      }else{
        KernelList[p->target_kernel].stat_sched_func(p->ldomain_shape, NHALO, yb, zb, p->lstencil_shape[0]+NHALO, ye, ze, p->coef, p->U2, p->U1, p->U3, p->stencil_ctx);
      }

      if((t)< p->t_dim){ // inverted trapezoid (or lower half of the diamond)
        yb -= b_inc;
        ye += e_inc;
      }else{ // trapezoid  (or upper half of the diamond)
        yb += b_inc;
        ye -= e_inc;
      }
    }
  }


  p->stencil_ctx.t_wf_prologue[tid] += t2-t1;
  p->stencil_ctx.t_wf_main[tid]     += t3-t2;
  p->stencil_ctx.t_wf_epilogue[tid] += MPI_Wtime() - t3;
}



// SEND TO LEFT
static inline void intra_diamond_strided_send_left(Parameters *p){
  ierr = MPI_Isend(p->U1, 1, p->hu[1].send_hb, p->t.down, 0, p->t.cart_comm, &(wait_req_send_l[0])); CHKERR(ierr);
  ierr = MPI_Isend(p->U2, 1, p->hv[1].send_hb, p->t.down, 0, p->t.cart_comm, &(wait_req_send_l[1])); CHKERR(ierr);
}
static inline void intra_diamond_concat_send_left(Parameters *p, FLOAT_PRECISION *send_buf){
  // concatenate the halo data then communicate contiguous data
  // assuming same halo size for both U and V buffers
  int h_size =  p->hu[1].size;
  int *offs;
  int i, j, k;
  int z_offs[] = {0,0,0};
  int h_offs[] = {h_size,0,0};

  if( p->t.down != MPI_PROC_NULL) {
    // copy the left halo of U to the buffer
    sub_array_copy_tg(p->U1, send_buf, p->ldomain_shape, p->hu[1].shape, p->hu[1].shape, p->hu[1].send_b, z_offs, p->stencil_ctx.thread_group_size);
    // copy the left halo of V to the buffer
    sub_array_copy_tg(p->U2, send_buf, p->ldomain_shape, p->hu[1].shape, p->hu[1].shape, p->hv[1].send_b, h_offs, p->stencil_ctx.thread_group_size);
  }
  // send the data out
  ierr = MPI_Isend(send_buf, 2*h_size, MPI_FLOAT_PRECISION, p->t.down , 0, p->t.cart_comm, &(wait_req_send_l[0])); CHKERR(ierr);
}
static inline void intra_diamond_send_left(Parameters *p, FLOAT_PRECISION *send_buf){
  if(p->halo_concat == 0){
    intra_diamond_strided_send_left(p);
  } else{
    // receive right side
    intra_diamond_concat_send_left(p, send_buf);
  }
}
static inline void intra_diamond_wait_send_left(Parameters *p){
  MPI_Status wait_stat[2];
  MPI_Status wait_stat1;
  if(p->halo_concat == 0){
    ierr = MPI_Waitall(2, wait_req_send_l, wait_stat); CHKERR(ierr); // send wait
  } else{
    ierr = MPI_Wait(&(wait_req_send_l[0]), &wait_stat1); CHKERR(ierr); // wait send left
  }
}


// SEND TO RIGHT
static inline void intra_diamond_strided_send_right(Parameters *p){
  ierr = MPI_Isend(p->U1, 1, p->hu[1].send_he, p->t.up, 0, p->t.cart_comm, &(wait_req_send_r[0])); CHKERR(ierr);
  ierr = MPI_Isend(p->U2, 1, p->hv[1].send_he, p->t.up, 0, p->t.cart_comm, &(wait_req_send_r[1])); CHKERR(ierr);
}
static inline void intra_diamond_concat_send_right(Parameters *p, FLOAT_PRECISION *send_buf){
  // concatenate the halo data then communicate contiguous data
  // assuming same halo size for both U and V buffers
  int h_size =  p->hu[1].size;
  int *offs;
  int i, j, k;
  int z_offs[] = {0,0,0};
  int h_offs[] = {h_size,0,0};

  if( p->t.up != MPI_PROC_NULL) {
    // copy the right halo of U to the buffer
    sub_array_copy_tg(p->U1, send_buf, p->ldomain_shape, p->hu[1].shape, p->hu[1].shape, p->hu[1].send_e, z_offs, p->stencil_ctx.thread_group_size);
    // copy the right halo of V to the buffer
    sub_array_copy_tg(p->U2, send_buf, p->ldomain_shape, p->hu[1].shape, p->hu[1].shape, p->hv[1].send_e, h_offs, p->stencil_ctx.thread_group_size);
  }
  // send the data out
  ierr = MPI_Isend(send_buf, 2*h_size, MPI_FLOAT_PRECISION, p->t.up , 0, p->t.cart_comm, &(wait_req_send_r[0])); CHKERR(ierr);
}
static inline void intra_diamond_send_right(Parameters *p, FLOAT_PRECISION *send_buf){
  if(p->halo_concat == 0){
    intra_diamond_strided_send_right(p);
  } else{
    intra_diamond_concat_send_right(p, send_buf);
 }
}
static inline void intra_diamond_wait_send_right(Parameters *p){
  MPI_Status wait_stat[2];
  MPI_Status wait_stat1;
  if(p->halo_concat == 0){
    ierr = MPI_Waitall(2, wait_req_send_r, wait_stat); CHKERR(ierr); // send wait
  } else{
    ierr = MPI_Wait(&(wait_req_send_r[0]), &wait_stat1); CHKERR(ierr); // wait send right
  }
}


// RECV FROM LEFT
static inline void intra_diamond_strided_recv_left(Parameters *p){
  ierr = MPI_Irecv(p->U1, 1, p->hu[1].recv_hb, p->t.down, MPI_ANY_TAG, p->t.cart_comm, &(wait_req_recv_l[0])); CHKERR(ierr);
  ierr = MPI_Irecv(p->U2, 1, p->hv[1].recv_hb, p->t.down, MPI_ANY_TAG, p->t.cart_comm, &(wait_req_recv_l[1])); CHKERR(ierr);
}
static inline void intra_diamond_recv_left(Parameters *p, FLOAT_PRECISION *recv_buf){
  if(p->halo_concat == 0){
    intra_diamond_strided_recv_left(p);
  } else{
     // receive left side
    ierr = MPI_Irecv(recv_buf, 2*p->hu[1].size, MPI_FLOAT_PRECISION, p->t.down , MPI_ANY_TAG, p->t.cart_comm, &(wait_req_recv_l[0])); CHKERR(ierr);
 }
}
static inline void intra_diamond_concat_wait_recv_left(Parameters *p, FLOAT_PRECISION *recv_buf){
  // assuming same halo size for both U and V buffers
  int h_size =  p->hu[1].size;
  int *offs;
  int i, j, k;
  int z_offs[] = {0,0,0};
  int h_offs[] = {h_size,0,0};
  MPI_Status wait_stat;

  // Complete receiving to copy the buffer data
  ierr = MPI_Wait(&(wait_req_recv_l[0]), &wait_stat); CHKERR(ierr);
  if( p->t.up != MPI_PROC_NULL) {
    // copy the receive buffer to the left halo of U
    sub_array_copy_tg(recv_buf, p->U1, p->hu[1].shape, p->ldomain_shape, p->hu[1].shape, z_offs, p->hu[1].recv_b, p->stencil_ctx.thread_group_size);
    // copy the receive buffer to the left halo of V
    sub_array_copy_tg(recv_buf, p->U2, p->hu[1].shape, p->ldomain_shape, p->hu[1].shape, h_offs, p->hv[1].recv_b, p->stencil_ctx.thread_group_size);
  }
}
static inline void intra_diamond_wait_recv_left(Parameters *p, FLOAT_PRECISION *recv_buf){
  MPI_Status wait_stat[2];
  MPI_Status wait_stat1;
  if(p->halo_concat == 0){
    ierr = MPI_Waitall(2, wait_req_recv_l, wait_stat); CHKERR(ierr); // receive wait
  } else{
    intra_diamond_concat_wait_recv_left(p, recv_buf);
  }
}


// RECV FROM RIGHT
static inline void intra_diamond_strided_recv_right(Parameters *p){
  ierr = MPI_Irecv(p->U1, 1, p->hu[1].recv_he, p->t.up, MPI_ANY_TAG, p->t.cart_comm, &(wait_req_recv_r[0])); CHKERR(ierr);
  ierr = MPI_Irecv(p->U2, 1, p->hv[1].recv_he, p->t.up, MPI_ANY_TAG, p->t.cart_comm, &(wait_req_recv_r[1])); CHKERR(ierr);
}
static inline void intra_diamond_recv_right(Parameters *p, FLOAT_PRECISION *recv_buf){
  if(p->halo_concat == 0){
    intra_diamond_strided_recv_right(p);
  } else{
    // receive right side
    ierr = MPI_Irecv(recv_buf, 2*p->hu[1].size, MPI_FLOAT_PRECISION, p->t.up , MPI_ANY_TAG, p->t.cart_comm, &(wait_req_recv_r[0])); CHKERR(ierr);
  }
}
static inline void intra_diamond_concat_wait_recv_right(Parameters *p, FLOAT_PRECISION *recv_buf){
  // assuming same halo size for both U and V buffers
  int h_size =  p->hu[1].size;
  int *offs;
  int i, j, k;
  int z_offs[] = {0,0,0};
  int h_offs[] = {h_size,0,0};
  MPI_Status wait_stat;

  // Complete receiving to copy the buffer data
  ierr = MPI_Wait(&(wait_req_recv_r[0]), &wait_stat); CHKERR(ierr);
  if( p->t.down != MPI_PROC_NULL) {
    // copy the receive buffer to the right halo of U
    sub_array_copy_tg(recv_buf, p->U1, p->hu[1].shape, p->ldomain_shape, p->hu[1].shape, z_offs, p->hu[1].recv_e, p->stencil_ctx.thread_group_size);
    // copy the receive buffer to the right halo of V
    sub_array_copy_tg(recv_buf, p->U2, p->hu[1].shape, p->ldomain_shape, p->hu[1].shape, h_offs, p->hv[1].recv_e, p->stencil_ctx.thread_group_size);
  }
}
static inline void intra_diamond_wait_recv_right(Parameters *p, FLOAT_PRECISION *recv_buf){
  MPI_Status wait_stat[2];
  if(p->halo_concat == 0){
    ierr = MPI_Waitall(2, wait_req_recv_r, wait_stat); CHKERR(ierr); // receive wait
  } else{
    intra_diamond_concat_wait_recv_right(p, recv_buf);
  }
}


// circular buffer
#define T_POS_L(y) (st.t_pos[(((y)+(y_len_l))%(y_len_l))])
#define T_POS_R(y) (st.t_pos[(((y)+(y_len_r))%(y_len_r))])
static inline void update_state(int y_coord, Parameters *p){
  int sh;
  st.t_pos[y_coord]++; // advance the current tile in time

  if(p->is_last != 1) {
    sh = ((st.t_pos[y_coord]%2 == 0) ? 1 : -1);// define the dependency direction

    // add the current tile to the ready queue if its dependency is satisfied
    if( (T_POS_L(y_coord+sh) >= st.t_pos[y_coord]) & (st.t_pos[y_coord] < t_len) )
    {
      avail_list[head%y_len_r] = y_coord;
      head++;
    }
    // add the dependent tile to the ready queue if its other dependency is satisfied
    if( (T_POS_L(y_coord-sh) == st.t_pos[y_coord]) & (T_POS_L(y_coord-sh) < t_len) )
    {
      avail_list[head%y_len_r] = (y_coord - sh + y_len_l)%y_len_l; // add the dependent neighbor to the list if the dependency is satisfied
      head++;
    }


  } else { // last process (and single process case)

    if(st.t_pos[y_coord]%2 == 0){ // right row case
      // add the current diamond to the ready queue if dependencies are satisfied
      if(st.t_pos[y_coord] < t_len){
        // if left-half diamond, no dependencies. Add to the list
        if(y_coord == y_len_l-1){
          avail_list[head%y_len_r] = y_coord;
          head++;
        } else if(T_POS_R(y_coord+1) >= st.t_pos[y_coord]) {
          //the reset have the same circular dependence (except the right-half diamond) if:
          // 1) the current tile did not reach the end of the temporal dimension
          // 2) the right neighbor is at least at the same time step
          avail_list[head%y_len_r] = y_coord;
          head++;
        }
      } // check validity in range of temporal dimension

      // add the dependent diamond to the ready queue if other dependencies are satisfied:
      if (T_POS_R(y_coord-1) < t_len){
        // add the right-half diamond automatically when the left most diamond is updated
        if(y_coord == 0){ // no dependencies. Add to the list
          st.t_pos[y_len_r-1]++; // advance the right-half diamond in time
          avail_list[head%y_len_r] = y_len_r-1;
          head++;
        }
        else if(T_POS_R(y_coord-1) == st.t_pos[y_coord]) {
          // 1) the neighbor did not reach the end of the temporal dimension
          // 2) the left neighbor is at the same time step
          // 3) is not the right-half diamond
          avail_list[head%y_len_r] = (y_coord - 1 + y_len_r)%y_len_r; // add the dependent neighbor to the list if the dependency is satisfied
          head++;
        }
      } // check validity in temporal dimension
    } //end right row case

    else if(st.t_pos[y_coord]%2 == 1){ // left row
      // add the current diamond to the ready queue if dependencies are satisfied:
      if( (T_POS_R(y_coord-1) >= st.t_pos[y_coord]) && (st.t_pos[y_coord] < t_len)  && (y_coord != y_len_r-1) ) {
        // 1) the left neighbor is at least at the same time step
        // 2) the current diamond did not reach the end of the temporal dimension
        // 3) is not the right-half diamond
        avail_list[head%y_len_r] = y_coord;
        head++;
      }

      // add the dependent diamond to the ready queue if other dependencies are satisfied:
      if( (T_POS_R(y_coord+1) == st.t_pos[y_coord]) && (T_POS_R(y_coord+1) < t_len) && (y_coord != y_len_l-1) ) {
        // 1) the right neighbor is at the same time step
        // 2) the neighbor did not reach the end of the temporal dimension
        // 3) is not the right most diamond in space
        avail_list[head%y_len_r] = (y_coord + 1 + y_len_r)%y_len_r; // add the dependent neighbor to the list if the dependency is satisfied
        head++;
      }
    } // end left row case

  } //end is_last process case
}

/*void comm_dead_lock_test(MPI_Request *req, int rank, int y_coord, int t_coord, char* source) {
  double db_t;
  int comm_test, comm_not_complete;
  MPI_Status wait_stat1;

  db_t= MPI_Wtime();
  comm_not_complete=1;
  while (comm_not_complete) {
    MPI_Test(&(req[0]), &comm_test, &wait_stat1);
    if(comm_test) comm_not_complete = 0;
    // assume deadlock if communication takes more than 10 seconds
    else if(MPI_Wtime()-db_t > 10){
      printf("[%d] DEADLOCK at %s wait t_pos[%d]=%d\n", rank, source, y_coord, t_coord);
      db_t = MPI_Wtime();
    }
  }
}*/
 
static inline void intra_diamond_get_info(Parameters *p, int y_coord, int tid, int t_coord, double *diam_size, int *yb, int *ye, int *b_inc, int *e_inc){
  if( (p->is_last == 1) && (y_coord == y_len_l-1) && (t_coord%2 == 0) ){ // 2nd right most process & left-half diamond
    // left half computations
    *yb = NHALO + p->lstencil_shape[1] - NHALO;
    *ye = *yb + NHALO;
    *b_inc = NHALO;
    *e_inc = 0;
    *diam_size = 0.5;
  }else if( (p->is_last == 1) && (y_coord == y_len_r-1) && (t_coord%2 == 0) ){ // right most process & right-half diamond
    // right half computations
    *b_inc = 0;
    *e_inc = NHALO;
    if(p->t.shape[1] > 1)
      *yb = NHALO + p->lstencil_shape[1] + 2*NHALO;
    else // serial code case
      *yb = NHALO;
    *ye = *yb + NHALO;
    *diam_size = 0.5;
  }else{ // full diamond computation
    if(t_coord%2 == 0)// row shifted to the right
      *yb = NHALO + diam_width - NHALO + y_coord*diam_width;
    else// row shifted to the left
      *yb = NHALO + diam_width/2 - NHALO+ y_coord*diam_width;
    *ye = *yb + 2*NHALO;
    *b_inc = NHALO;
    *e_inc = NHALO;
    *diam_size = 1.0;
  }
}

static inline void intra_diamond_comm(Parameters *p, int y_coord, int t_coord){
  // Start exchanging computed halo data
  if(p->t.shape[1] > 1){
    if( (y_coord == y_len_r-1) && (t_coord%2 == 0) ) { // right most diamond
      intra_diamond_send_right(p, send_buf_r);
      intra_diamond_recv_left (p, recv_buf_l);

//      comm_dead_lock_test(wait_req_send_r, p->mpi_rank, y_coord, t_coord, "send right");
      intra_diamond_wait_send_right(p);

//      comm_dead_lock_test(wait_req_recv_l, p->mpi_rank, y_coord, t_coord, "recv left");
      intra_diamond_wait_recv_left (p, recv_buf_l);


    } else if( (y_coord == 0) && (t_coord%2 == 1) ){ // left  most diamond
      intra_diamond_send_left (p, send_buf_l);
      intra_diamond_recv_right(p, recv_buf_r);

//      comm_dead_lock_test(wait_req_send_l, p->mpi_rank, y_coord, t_coord, "send left");
      intra_diamond_wait_send_left (p);

//      comm_dead_lock_test(wait_req_recv_r, p->mpi_rank, y_coord, t_coord, "recv right");
      intra_diamond_wait_recv_right(p, recv_buf_r);
    }
  }
}

static inline void intra_diamond_resolve(Parameters *p, int y_coord, int tid){
  int t_coord = st.t_pos[y_coord];
  int yb, ye;
  int b_inc, e_inc;
  double diam_size;
//  t1 = MPI_Wtime();
  double t1, t2;

  intra_diamond_get_info(p, y_coord, tid, t_coord, &diam_size, &yb, &ye, &b_inc, &e_inc);
  p->stencil_ctx.wf_num_resolved_diamonds[tid] += diam_size;

  if(p->wavefront == 0){
    intra_diamond_comp(p, yb, ye, 1, b_inc, e_inc);
  } else {
    if(p->wavefront == 1){
      intra_diamond_1wf_comp(p, yb, ye, b_inc, e_inc);
    } else {
      intra_diamond_all_mwf_comp(p, yb, ye, b_inc, e_inc, tid);
    }
  }

  t1 = MPI_Wtime();
  intra_diamond_comm(p, y_coord, t_coord);
  t2 = MPI_Wtime();
  p->stencil_ctx.t_wf_comm[tid] += t2-t1;
}


void dynamic_intra_diamond_main_loop(Parameters *p){
  int not_complete, th_y_coord, i;
  unsigned long int il;
  int num_thread_groups = (int) ceil(1.0*p->num_threads/p->stencil_ctx.thread_group_size);
  unsigned long int diam_size = y_len_l*(t_len-1)/2 + y_len_r*((t_len-1)/2 +1);
  int tid;
  double t1;

  for(i=0; i<y_len_r; i++){
    avail_list[i] = i;
  }

#pragma omp parallel num_threads(num_thread_groups) shared(head, tail) private(tid)
  {
    // initlaize the likwid markers according to the openmp nested parallelism
    #pragma omp parallel num_threads(p->stencil_ctx.thread_group_size)
    {
      if(p->stencil_ctx.enable_likwid_m == 1) { 
        LIKWID_MARKER_THREADINIT;
        LIKWID_MARKER_START("calc"); 
      }
    }

   tid = 0;
#if defined(_OPENMP)
    tid = omp_get_thread_num();
#endif


#pragma omp for schedule(dynamic) private(il, th_y_coord, not_complete)//shared(head,tail)
    for (il=0; il<diam_size; il++){

      not_complete = 1;
      th_y_coord = -1;
      while(not_complete)
      {
        t1 = MPI_Wtime();
        while(head-tail<1); // spin-wait for available tasks
        p->stencil_ctx.t_group_wait[tid] += (MPI_Wtime() - t1);

#pragma omp critical// (consumer)
        {
#pragma omp flush (head, tail)
          if(head-tail>0){ // make sure there is still available work
            th_y_coord = avail_list[tail%y_len_r]; //acquire task
            tail++;
          }
        }
        if(th_y_coord>=0){
          intra_diamond_resolve(p, th_y_coord, tid);
#pragma omp critical// (producer)
          {
#pragma omp flush (head)
            update_state(th_y_coord, p);
          }
          not_complete = 0;
        }
      }
    }
    // stop the markers of the experiment
    #pragma omp parallel num_threads(p->stencil_ctx.thread_group_size)
    { 
      if(p->stencil_ctx.enable_likwid_m == 1) {
        LIKWID_MARKER_STOP("calc"); 
      }
    }

  }
}


void dynamic_intra_diamond_ts(Parameters *p) {

  int t_dim = p->t_dim;
  diam_width = (t_dim+1) * 2 *NHALO;
  t_len = 2*( (p->nt-2)/((t_dim+1)*2) ) - 1;
  y_len_l = p->lstencil_shape[1] / (diam_width);
  y_len_r = y_len_l;
  if(p->is_last == 1) y_len_r++;

  int i, y, t;
  double t1,t2,t3,t4;
  int yb,ye;
  //  MPI_Request wait_req[4];
  double db_t;

  // allocate scheduling variables
  st.t_pos = (int*) malloc(y_len_r*sizeof(int));
  st.state = (int*) malloc(y_len_r*sizeof(int));
  avail_list = (int*) malloc(y_len_r*sizeof(int));
  head=y_len_r;
  tail=0;
  // initialize scheduling variables
  for(i=0; i<y_len_r; i++){
    st.t_pos[i] = 0;
    st.state[i] = ST_NOT_BUSY;
  }

  // create buffers to aggregate halo data for communication
  //  FLOAT_PRECISION *send_buf, *recv_buf;
  int comm_buf_size;
  if (p->halo_concat ==1){
    // assuming same halo size for both U and V buffers
    comm_buf_size = 2 * p->hu[1].shape[0] * p->hu[1].shape[1] * p->hu[1].shape[2];
    posix_memalign((void **)&(recv_buf_l), p->alignment, sizeof(FLOAT_PRECISION)*comm_buf_size);
    posix_memalign((void **)&(recv_buf_r), p->alignment, sizeof(FLOAT_PRECISION)*comm_buf_size);
    posix_memalign((void **)&(send_buf_l), p->alignment, sizeof(FLOAT_PRECISION)*comm_buf_size);
    posix_memalign((void **)&(send_buf_r), p->alignment, sizeof(FLOAT_PRECISION)*comm_buf_size);
  }


  // Prologue
  t1 = MPI_Wtime();
  // compute all the trapezoids
  yb = NHALO; ye = yb + diam_width;
  for(i=0; i<y_len_l; i++){
    intra_diamond_trapzd_comp(p, yb, ye);
    yb += diam_width; ye+= diam_width;
  }
//  t2 = MPI_Wtime();
  // Send the trapezoid results to the left
  if(p->t.shape[1] > 1){
    intra_diamond_send_left (p, send_buf_l);
    intra_diamond_recv_right(p, recv_buf_r);

    intra_diamond_wait_send_left (p);
    intra_diamond_wait_recv_right(p, recv_buf_r);
  }
//  t3 = MPI_Wtime();
//  p->prof.compute     += (t2-t1);
//  p->prof.communicate += (t3-t2);

#if defined(_OPENMP)
  omp_set_nested(1);
#endif

  t2 = MPI_Wtime();
  // main loop
  dynamic_intra_diamond_main_loop(p);
  t3 = MPI_Wtime();

  // Epilogue

//  t1 = MPI_Wtime();
  yb = NHALO + diam_width/2 - NHALO; ye = yb + 2*NHALO;
  for(i=0; i<y_len_l; i++){
    intra_diamond_inv_trapzd_comp(p, p->nt - (t_dim+2), yb, ye);
    yb += diam_width; ye+= diam_width;
  }
  t4 = MPI_Wtime();

  p->prof.ts_main += (t3-t2);
  p->prof.ts_others += (t2-t1) + (t4-t3);


  // clean up the buffers that aggregate halo data for communication
  if (p->halo_concat ==1){
    free(recv_buf_l);
    free(recv_buf_r);
    free(send_buf_l);
    free(send_buf_r);
  }
  // cleanup the state variables
  free((void *) st.t_pos);
  free(st.state);
  free((void *) avail_list);

}
