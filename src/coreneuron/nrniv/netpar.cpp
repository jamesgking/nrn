/*
Copyright (c) 2014 EPFL-BBP, All rights reserved.

THIS SOFTWARE IS PROVIDED BY THE BLUE BRAIN PROJECT "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE BLUE BRAIN PROJECT
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <map>
#include "coreneuron/nrnconf.h"
#include "coreneuron/nrnoc/multicore.h"
#include "coreneuron/nrnmpi/nrnmpi.h"
#include "coreneuron/nrniv/nrn_assert.h"

#undef MD
#define MD 2147483648.

class PreSyn;
class InputPreSyn;

#include "coreneuron/nrniv/netcon.h"
#include "coreneuron/nrniv/netcvode.h"
#include "coreneuron/nrniv/nrniv_decl.h"

#define BGP_INTERVAL 2
#if BGP_INTERVAL == 2
static int n_bgp_interval;
#endif

static double t_exchange_;
static double dt1_; // 1/dt
static void alloc_space();

/// Vector of maps for negative presyns
std::vector< std::map<int, PreSyn*> > neg_gid2out;
/// Maps for ouput and input presyns
std::map<int, PreSyn*> gid2out;
std::map<int, InputPreSyn*> gid2in;

extern "C" {
extern NetCvode* net_cvode_instance;
extern double t, dt;
extern int nrn_use_selfqueue_;
extern int vector_capacity(IvocVect*); //ivocvect.h conflicts with STL
extern double* vector_vec(IvocVect*);
extern void nrn_fake_fire(int gid, double firetime, int fake_out);
int nrnmpi_spike_compress(int nspike, bool gid_compress, int xchng_meth);
void nrn_spike_exchange_init();
extern double nrn_bgp_receive_time(int);

double nrn_bgp_receive_time(int) { return 0.; }

}

static double set_mindelay(double maxdelay);

#if NRNMPI

#include "coreneuron/nrnmpi/mpispike.h"

extern "C" {
void nrn_timeout(int);
void nrn_spike_exchange(NrnThread*);
extern int nrnmpi_int_allmax(int);
extern void nrnmpi_int_allgather(int*, int*, int);
void nrn2ncs_outputevent(int netcon_output_index, double firetime);
}


void nrnmpi_gid_clear(void)
{
  gid2in.clear();
  gid2out.clear();
}

// for compressed gid info during spike exchange
bool nrn_use_localgid_;
void nrn_outputevent(unsigned char localgid, double firetime);
std::vector< std::map<int, InputPreSyn*> > localmaps;

#define NRNSTAT 1
static int nsend_, nsendmax_, nrecv_, nrecv_useful_;
#if NRNSTAT
static IvocVect* max_histogram_;
#endif 

static int ocapacity_; // for spikeout_
// require it to be smaller than  min_interprocessor_delay.
static double wt_; // wait time for nrnmpi_spike_exchange
static double wt1_; // time to find the PreSyns and send the spikes.
static bool use_compress_;
static int spfixout_capacity_;
static int idxout_;
static void nrn_spike_exchange_compressed(NrnThread*);
#endif // NRNMPI

#define HAVE_DCMF_RECORD_REPLAY 0

static int active_;
static double usable_mindelay_;
static double min_interprocessor_delay_;
static double mindelay_; // the one actually used. Some of our optional algorithms
static double last_maxstep_arg_;
static NetParEvent* npe_; // nrn_nthread of them
static int n_npe_; // just to compare with nrn_nthread

#if NRNMPI
// for combination of threads and mpi.
#if (USE_PTHREAD || defined(_OPENMP))
static MUTDEC
#endif
#endif

NetParEvent::NetParEvent(){
	wx_ = ws_ = 0.;
	ithread_ = -1;
}
NetParEvent::~NetParEvent(){
}
void NetParEvent::send(double tt, NetCvode* nc, NrnThread* nt){
	nc->event(tt + usable_mindelay_, this, nt);
}
void NetParEvent::deliver(double tt, NetCvode* nc, NrnThread* nt){
	if (nrn_use_selfqueue_) { //first handle pending flag=1 self events
		nrn_pending_selfqueue(tt, nt);
	}
	net_cvode_instance->deliver_events(tt, nt);
	nt->_stop_stepping = 1;
	nt->_t = tt;
	send(tt, nc, nt);
}

void nrn_netparevent_finish_deliver() {
	NrnThread* nt = nrn_threads;  
	nrn_spike_exchange(nt);
}

void NetParEvent::pr(const char* m, double tt, NetCvode*){
	printf("%s NetParEvent %d t=%.15g tt-t=%g\n", m, ithread_, tt, tt - nrn_threads[ithread_]._t);
}

#if NRNMPI
inline static void sppk(unsigned char* c, int gid) {
	for (int i = localgid_size_-1; i >= 0; --i) {
		c[i] = gid & 255;
		gid >>= 8;
	}
}
inline static int spupk(unsigned char* c) {
	int gid = *c++;
	for (int i = 1; i < localgid_size_; ++i) {
		gid <<= 8;
		gid += *c++;
	}
	return gid;
}

void nrn_outputevent(unsigned char localgid, double firetime) {
	if (!active_) { return; }
	MUTLOCK
	nout_++;
	int i = idxout_;
	idxout_ += 2;
	if (idxout_ >= spfixout_capacity_) {
		spfixout_capacity_ *= 2;
		spfixout_ = (unsigned char*)erealloc(spfixout_, spfixout_capacity_*sizeof(unsigned char));
	}
	spfixout_[i++] = (unsigned char)((firetime - t_exchange_)*dt1_ + .5);
	spfixout_[i] = localgid;
//printf("%d idx=%d lgid=%d firetime=%g t_exchange_=%g [0]=%d [1]=%d\n", nrnmpi_myid, i, (int)localgid, firetime, t_exchange_, (int)spfixout_[i-1], (int)spfixout_[i]);
	MUTUNLOCK
}

void nrn2ncs_outputevent(int gid, double firetime) {
	if (!active_) { return; }
	MUTLOCK
    if (use_compress_) {
	nout_++;
	int i = idxout_;
	idxout_ += 1 + localgid_size_;
	if (idxout_ >= spfixout_capacity_) {
		spfixout_capacity_ *= 2;
		spfixout_ = (unsigned char*)erealloc(spfixout_, spfixout_capacity_*sizeof(unsigned char));
	}
//printf("%d nrnncs_outputevent %d %.20g %.20g %d\n", nrnmpi_myid, gid, firetime, t_exchange_,
//(int)((unsigned char)((firetime - t_exchange_)*dt1_ + .5)));
	spfixout_[i++] = (unsigned char)((firetime - t_exchange_)*dt1_ + .5);
//printf("%d idx=%d firetime=%g t_exchange_=%g spfixout=%d\n", nrnmpi_myid, i, firetime, t_exchange_, (int)spfixout_[i-1]);
	sppk(spfixout_+i, gid);
//printf("%d idx=%d gid=%d spupk=%d\n", nrnmpi_myid, i, gid, spupk(spfixout_+i));
    }else{
#if nrn_spikebuf_size == 0
	int i = nout_++;
	if (i >= ocapacity_) {
		ocapacity_ *= 2;
		spikeout_ = (NRNMPI_Spike*)erealloc(spikeout_, ocapacity_*sizeof(NRNMPI_Spike));
	}		
//printf("%d cell %d in slot %d fired at %g\n", nrnmpi_myid, gid, i, firetime);
	spikeout_[i].gid = gid;
	spikeout_[i].spiketime = firetime;
#else
	int i = nout_++;
	if (i >= nrn_spikebuf_size) {
		i -= nrn_spikebuf_size;
		if (i >= ocapacity_) {
			ocapacity_ *= 2;
			spikeout_ = (NRNMPI_Spike*)hoc_Erealloc(spikeout_, ocapacity_*sizeof(NRNMPI_Spike)); hoc_malchk();
		}		
		spikeout_[i].gid = gid;
		spikeout_[i].spiketime = firetime;
	}else{
		spbufout_->gid[i] = gid;
		spbufout_->spiketime[i] = firetime;
	}
#endif
    }
	MUTUNLOCK
//printf("%d cell %d in slot %d fired at %g\n", nrnmpi_myid, gid, i, firetime);
}
#endif // NRNMPI

static int nrn_need_npe() {
	
	int b = 0;
	if (active_) { b = 1; }
	if (nrn_use_selfqueue_) { b = 1; }
	if (nrn_nthread > 1) { b = 1; }
	if (b) {
		if (last_maxstep_arg_ == 0) {
			last_maxstep_arg_ =   100.;
		}
		set_mindelay(last_maxstep_arg_);
	}else{
		if (npe_) {
			delete [] npe_;
			npe_ = nil;
			n_npe_ = 0;
		}
	}
	return b;
}

#define TBUFSIZE 0
#define TBUF /**/

void nrn_spike_exchange_init() {
//printf("nrn_spike_exchange_init\n");
	if (!nrn_need_npe()) { return; }
//	if (!active_ && !nrn_use_selfqueue_) { return; }
	alloc_space();
//printf("nrnmpi_use=%d active=%d\n", nrnmpi_use, active_);
    std::map<int, InputPreSyn*>::iterator gid2in_it;
	usable_mindelay_ = mindelay_;
	if (nrn_nthread > 1) {
		usable_mindelay_ -= dt;
	}
	if ((usable_mindelay_ < 1e-9) || (usable_mindelay_ < dt)) {
		if (nrnmpi_myid == 0) {
			hoc_execerror("usable mindelay is 0", "(or less than dt for fixed step method)");
		}else{
			return;
		}
	}

#if TBUFSIZE
		itbuf_ = 0;
#endif

	if (n_npe_ != nrn_nthread) {
		if (npe_) { delete [] npe_; }
		npe_ = new NetParEvent[nrn_nthread];
		n_npe_ = nrn_nthread;
	}
	for (int i = 0; i < nrn_nthread; ++i) {
		npe_[i].ithread_ = i;
		npe_[i].wx_ = 0.;
		npe_[i].ws_ = 0.;
		npe_[i].send(t, net_cvode_instance, nrn_threads + i);
	}
#if NRNMPI
    if (use_compress_) {
	idxout_ = 2;
	t_exchange_ = t;
	dt1_ = 1./dt;
	usable_mindelay_ = floor(mindelay_ * dt1_ + 1e-9) * dt;
	assert (usable_mindelay_ >= dt && (usable_mindelay_ * dt1_) < 255);
    }else{
#if nrn_spikebuf_size > 0
	if (spbufout_) {
		spbufout_->nspike = 0;
	}
#endif
    }
	nout_ = 0;
	nsend_ = nsendmax_ = nrecv_ = nrecv_useful_ = 0;
	if (nrnmpi_numprocs > 0) {
		if (nrn_nthread > 0) {
#if (USE_PTHREAD || defined(_OPENMP))
			if (!mut_) {
				MUTCONSTRUCT(1)
			}
#endif
		}else{
			MUTDESTRUCT
		}
	}
#endif // NRNMPI
	//if (nrnmpi_myid == 0){printf("usable_mindelay_ = %g\n", usable_mindelay_);}
}

#if NRNMPI
void nrn_spike_exchange(NrnThread* nt) {
	if (!active_) { return; }
	if (use_compress_) { nrn_spike_exchange_compressed(nt); return; }
	TBUF
#if TBUFSIZE
	nrnmpi_barrier();
#endif
	TBUF
	double wt;
	int i, n;
    std::map<int, InputPreSyn*>::iterator gid2in_it;
#if NRNSTAT
	nsend_ += nout_;
	if (nsendmax_ < nout_) { nsendmax_ = nout_; }
#endif
#if nrn_spikebuf_size > 0
	spbufout_->nspike = nout_;
#endif
	wt = nrnmpi_wtime();

	n = nrnmpi_spike_exchange();

	wt_ = nrnmpi_wtime() - wt;
	wt = nrnmpi_wtime();
	TBUF
#if TBUFSIZE
	tbuf_[itbuf_++] = (unsigned long)nout_;
	tbuf_[itbuf_++] = (unsigned long)n;
#endif

	errno = 0;
//if (n > 0) {
//printf("%d nrn_spike_exchange sent %d received %d\n", nrnmpi_myid, nout_, n);
//}
	nout_ = 0;
	if (n == 0) {
#if NRNSTAT
		if (max_histogram_) { vector_vec(max_histogram_)[0] += 1.; }
#endif
		TBUF
		return;
	}
#if NRNSTAT
	nrecv_ += n;
	if (max_histogram_) {
		int mx = 0;
		if (n > 0) {
			for (i=nrnmpi_numprocs-1 ; i >= 0; --i) {
#if nrn_spikebuf_size == 0
				if (mx < nin_[i]) {
					mx = nin_[i];
				}
#else
				if (mx < spbufin_[i].nspike) {
					mx = spbufin_[i].nspike;
				}
#endif
			}
		}
		int ms = vector_capacity(max_histogram_)-1;
		mx = (mx < ms) ? mx : ms;
		vector_vec(max_histogram_)[mx] += 1.;
	}
#endif // NRNSTAT
#if nrn_spikebuf_size > 0
	for (i = 0; i < nrnmpi_numprocs; ++i) {
		int j;
		int nn = spbufin_[i].nspike;
		if (nn > nrn_spikebuf_size) { nn = nrn_spikebuf_size; }
		for (j=0; j < nn; ++j) {
            gid2in_it = gid2in.find(spbufin_[i].gid[j]);
            if (gid2in_it != gid2in.end()) {
                InputPreSyn* ps = gid2in_it->second;
                ps->send(spbufin_[i].spiketime[j], net_cvode_instance, nt);
#if NRNSTAT
				++nrecv_useful_;
#endif
			}
		}
	}
	n = ovfl_;
#endif // nrn_spikebuf_size > 0
	for (i = 0; i < n; ++i) {
        gid2in_it = gid2in.find(spikein_[i].gid);
        if (gid2in_it != gid2in.end()) {
            InputPreSyn* ps = gid2in_it->second;
			ps->send(spikein_[i].spiketime, net_cvode_instance, nt);
#if NRNSTAT
			++nrecv_useful_;
#endif
		}
	}
	wt1_ = nrnmpi_wtime() - wt;
	TBUF
}
		
void nrn_spike_exchange_compressed(NrnThread* nt) {
	if (!active_) { return; }
	TBUF
#if TBUFSIZE
	nrnmpi_barrier();
#endif
	TBUF
	double wt;
	int i, n, idx;
    std::map<int, InputPreSyn*>::iterator gid2in_it;
#if NRNSTAT
	nsend_ += nout_;
	if (nsendmax_ < nout_) { nsendmax_ = nout_; }
#endif
	assert(nout_ < 0x10000);
	spfixout_[1] = (unsigned char)(nout_ & 0xff);
	spfixout_[0] = (unsigned char)(nout_>>8);

	wt = nrnmpi_wtime();
	n = nrnmpi_spike_exchange_compressed();
	wt_ = nrnmpi_wtime() - wt;
	wt = nrnmpi_wtime();
	TBUF
#if TBUFSIZE             
        tbuf_[itbuf_++] = (unsigned long)nout_;
        tbuf_[itbuf_++] = (unsigned long)n;
#endif
	errno = 0;
//if (n > 0) {
//printf("%d nrn_spike_exchange sent %d received %d\n", nrnmpi_myid, nout_, n);
//}
	nout_ = 0;
	idxout_ = 2;
	if (n == 0) {
#if NRNSTAT
		if (max_histogram_) { vector_vec(max_histogram_)[0] += 1.; }
#endif
		t_exchange_ = nrn_threads->_t;
		TBUF
		return;
	}
#if NRNSTAT
	nrecv_ += n;
	if (max_histogram_) {
		int mx = 0;
		if (n > 0) {
			for (i=nrnmpi_numprocs-1 ; i >= 0; --i) {
				if (mx < nin_[i]) {
					mx = nin_[i];
				}
			}
		}
		int ms = vector_capacity(max_histogram_)-1;
		mx = (mx < ms) ? mx : ms;
		vector_vec(max_histogram_)[mx] += 1.;
	}
#endif // NRNSTAT
    if (nrn_use_localgid_) {
	int idxov = 0;
	for (i = 0; i < nrnmpi_numprocs; ++i) {
		int j, nnn;
		int nn = nin_[i];
	    if (nn) {
		if (i == nrnmpi_myid) { // skip but may need to increment idxov.
			if (nn > ag_send_nspike_) {
				idxov += (nn - ag_send_nspike_)*(1 + localgid_size_);
			}
			continue;
		}
        std::map<int, InputPreSyn*> gps = localmaps[i];
		if (nn > ag_send_nspike_) {
			nnn = ag_send_nspike_;
		}else{
			nnn = nn;
		}
		idx = 2 + i*ag_send_size_;
		for (j=0; j < nnn; ++j) {
			// order is (firetime,gid) pairs.
			double firetime = spfixin_[idx++]*dt + t_exchange_;
			int lgid = (int)spfixin_[idx];
			idx += localgid_size_;
            gid2in_it = gps.find(lgid);
            if (gid2in_it != gps.end()) {
                InputPreSyn* ps = gid2in_it->second;
				ps->send(firetime + 1e-10, net_cvode_instance, nt);
#if NRNSTAT
				++nrecv_useful_;
#endif
			}
		}
		for ( ; j < nn; ++j) {
			double firetime = spfixin_ovfl_[idxov++]*dt + t_exchange_;
			int lgid = (int)spfixin_ovfl_[idxov];
			idxov += localgid_size_;
            gid2in_it = gps.find(lgid);
            if (gid2in_it != gps.end()) {
                InputPreSyn* ps = gid2in_it->second;
				ps->send(firetime+1e-10, net_cvode_instance, nt);
#if NRNSTAT
				++nrecv_useful_;
#endif
			}
		}
	    }
	}
    }else{
	for (i = 0; i < nrnmpi_numprocs; ++i) {
		int j;
		int nn = nin_[i];
		if (nn > ag_send_nspike_) { nn = ag_send_nspike_; }
		idx = 2 + i*ag_send_size_;
		for (j=0; j < nn; ++j) {
			// order is (firetime,gid) pairs.
			double firetime = spfixin_[idx++]*dt + t_exchange_;
			int gid = spupk(spfixin_ + idx);
			idx += localgid_size_;
            gid2in_it = gid2in.find(gid);
            if (gid2in_it != gid2in.end()) {
                InputPreSyn* ps = gid2in_it->second;
				ps->send(firetime+1e-10, net_cvode_instance, nt);
#if NRNSTAT
				++nrecv_useful_;
#endif
			}
		}
	}
	n = ovfl_;
	idx = 0;
	for (i = 0; i < n; ++i) {
		double firetime = spfixin_ovfl_[idx++]*dt + t_exchange_;
		int gid = spupk(spfixin_ovfl_ + idx);
		idx += localgid_size_;
        gid2in_it = gid2in.find(gid);
        if (gid2in_it != gid2in.end()) {
            InputPreSyn* ps = gid2in_it->second;
			ps->send(firetime+1e-10, net_cvode_instance, nt);
#if NRNSTAT
			++nrecv_useful_;
#endif
		}
	}
    }
	t_exchange_ = nrn_threads->_t;
	wt1_ = nrnmpi_wtime() - wt;
	TBUF
}

static void mk_localgid_rep() {
    int i, k;

	// how many gids are there on this machine
	// and can they be compressed into one byte
	int ngid = 0;
    std::map<int, PreSyn*>::iterator gid2out_it;
    std::map<int, InputPreSyn*>::iterator gid2in_it;
    for(gid2out_it = gid2out.begin(); gid2out_it != gid2out.end(); ++gid2out_it) {
        if (gid2out_it->second->output_index_ >= 0) {
			++ngid;
		}
	}

	int ngidmax = nrnmpi_int_allmax(ngid);
	if (ngidmax > 256) {
		//do not compress
		return;
	}
	localgid_size_ = sizeof(unsigned char);
	nrn_use_localgid_ = true;

	// allocate Allgather receive buffer (send is the nrnmpi_myid one)
	int* rbuf = new int[nrnmpi_numprocs*(ngidmax + 1)];
	int* sbuf = new int[ngidmax + 1];

	sbuf[0] = ngid;
	++sbuf;
	ngid = 0;
	// define the local gid and fill with the gids on this machine
    for(gid2out_it = gid2out.begin(); gid2out_it != gid2out.end(); ++gid2out_it) {
        if (gid2out_it->second->output_index_ >= 0) {
            gid2out_it->second->localgid_ = (unsigned char)ngid;
            sbuf[ngid] = gid2out_it->second->output_index_;
			++ngid;
		}
	}
	--sbuf;

	// exchange everything
	nrnmpi_int_allgather(sbuf, rbuf, ngidmax+1);
	delete [] sbuf;
	errno = 0;

	// create the maps
	// there is a lot of potential for efficiency here. i.e. use of
	// perfect hash functions, or even simple Vectors.
    localmaps.clear();
    localmaps.resize(nrnmpi_numprocs);

	// fill in the maps
	for (i = 0; i < nrnmpi_numprocs; ++i) if (i != nrnmpi_myid) {
		sbuf = rbuf + i*(ngidmax + 1);
		ngid = *(sbuf++);
		for (k=0; k < ngid; ++k) {
            gid2in_it = gid2in.find(int(sbuf[k]));
            if (gid2in_it != gid2in.end()) {
                localmaps[i][k] = gid2in_it->second;
			}
		}
	}

	// cleanup
	delete [] rbuf;
}

#endif // NRNMPI

// may stimulate a gid for a cell not owned by this cpu. This allows
// us to run single cells or subnets and stimulate exactly according to
// their input in a full parallel net simulation.
// For some purposes, it may be useful to simulate a spike from a
// cell that does exist and would normally send its own spike, eg.
// recurrent stimulation. This can be useful in debugging where the
// spike raster comes from another implementation and one wants to
// get complete control of all input spikes without the confounding
// effects of output spikes from the simulated cells. In this case
// set the third arg to 1 and set the output cell thresholds very
// high so that they do not themselves generate spikes.
// Can only be called by thread 0 because of the ps->send.
void nrn_fake_fire(int gid, double spiketime, int fake_out) {
    std::map<int, InputPreSyn*>::iterator gid2in_it;
    gid2in_it = gid2in.find(gid);
    if (gid2in_it != gid2in.end())
    {
        InputPreSyn* psi = gid2in_it->second;
        assert(psi);
//printf("nrn_fake_fire %d %g\n", gid, spiketime);
        psi->send(spiketime, net_cvode_instance, nrn_threads);
#if NRNSTAT
        ++nrecv_useful_;
#endif
    }else if (fake_out)
    {
        std::map<int, PreSyn*>::iterator gid2out_it;
        gid2out_it = gid2out.find(gid);
        if (gid2out_it != gid2out.end())
        {
          PreSyn* ps = gid2out_it->second;
          assert(ps);
//printf("nrn_fake_fire fake_out %d %g\n", gid, spiketime);
          ps->send(spiketime, net_cvode_instance, nrn_threads);
#if NRNSTAT
          ++nrecv_useful_;
#endif
        }
    }

}


void netpar_tid_gid2ps_alloc(int nth) {
  // nth is same as ngroup in nrn_setup.cpp, not necessarily nrn_nthread.
  neg_gid2out.resize(nth);
}

void netpar_tid_gid2ps_free() {
  neg_gid2out.clear();
}

void netpar_tid_gid2ps(int tid, int gid, PreSyn** ps, InputPreSyn** psi){
  /// for gid < 0 returns the PreSyn* in the thread (tid) specific map.
  *ps = NULL;
  *psi = NULL;
  std::map<int, PreSyn*>::iterator gid2out_it;
  if (gid >= 0) {
      gid2out_it = gid2out.find(gid);
      if (gid2out_it != gid2out.end()) {
        *ps = gid2out_it->second;
      }else{
        std::map<int, InputPreSyn*>::iterator gid2in_it;
        gid2in_it = gid2in.find(gid);
        if (gid2in_it != gid2in.end()) {
          *psi = gid2in_it->second;
        }
      }
  }else{
    gid2out_it = neg_gid2out[tid].find(gid);
    if (gid2out_it != neg_gid2out[tid].end()) {
      *ps = gid2out_it->second;
    }
  }
}
  
void netpar_tid_set_gid2node(int tid, int gid, int nid, PreSyn* ps) {
  if (gid >= 0) {
      /// Allocate space for spikes: 200 structs of {int gid; double time}
      /// coming from nrnmpi.h and array of int of the global domain size
      alloc_space();
#if NRNMPI
      if (nid == nrnmpi_myid) {
#else
      {
#endif
          char m[200];
          if (gid2in.find(gid) != gid2in.end()) {
              sprintf(m, "gid=%d already exists as an input port", gid);
              hoc_execerror(m, "Setup all the output ports on this process before using them as input ports.");
          }
          if (gid2out.find(gid) != gid2out.end()) {
              sprintf(m, "gid=%d already exists on this process as an output port", gid);
              hoc_execerror(m, 0);
          }
          gid2out[gid] = ps;
          ps->gid_ = gid;
          ps->output_index_ = gid;
      }
  }else{
    nrn_assert(nid == nrnmpi_myid);
    nrn_assert(neg_gid2out[tid].find(gid) == neg_gid2out[tid].end());
    neg_gid2out[tid][gid] = ps;
  }
}


void nrn_reset_gid2out(void) {
  gid2out.clear();
}

void nrn_reset_gid2in(void) {
  gid2in.clear();
}

static void alloc_space() {
#if NRNMPI
	if (!spikeout_) {
		ocapacity_  = 100;
		spikeout_ = (NRNMPI_Spike*)emalloc(ocapacity_*sizeof(NRNMPI_Spike));
		icapacity_  = 100;
		spikein_ = (NRNMPI_Spike*)malloc(icapacity_*sizeof(NRNMPI_Spike));
		nin_ = (int*)emalloc(nrnmpi_numprocs*sizeof(int));
#if nrn_spikebuf_size > 0
spbufout_ = (NRNMPI_Spikebuf*)emalloc(sizeof(NRNMPI_Spikebuf));
spbufin_ = (NRNMPI_Spikebuf*)emalloc(nrnmpi_numprocs*sizeof(NRNMPI_Spikebuf));
#endif
#endif
	}
}


void nrn_cleanup_presyn(DiscreteEvent*) {
    // for multi-send, need to cleanup the list of hosts here
}


int input_gid_register(int gid) {
	alloc_space();
    if (gid2out.find(gid) != gid2out.end()) {
		return 0;
    }else if (gid2in.find(gid) != gid2in.end()) {
		return 0;
	}
    gid2in[gid] = NULL;
	return 1;
}

int input_gid_associate(int gid, InputPreSyn* psi) {
    std::map<int, InputPreSyn*>::iterator gid2in_it;
    gid2in_it = gid2in.find(gid);
    if (gid2in_it != gid2in.end()) {
        if (gid2in_it->second) {
			return 0;
		}
        gid2in_it->second = psi;
                psi->gid_ = gid;
		return 1;
	}
	return 0;
}


static int timeout_ = 0;
int nrn_set_timeout(int timeout) {
	int tt;
	tt = timeout_;
	timeout_ = timeout;
	return tt;
}

void BBS_netpar_solve(double tstop) {

        double time = nrnmpi_wtime();

#if NRNMPI
	double mt, md;
	tstopunset;
	mt = dt ; md = mindelay_ - 1e-10;
	if (md < mt) {
		if (nrnmpi_myid == 0) {
			hoc_execerror("mindelay is 0", "(or less than dt for fixed step method)");
		}else{
			return;
		}
	}
//	double wt;

	nrn_timeout(timeout_);
//	wt = nrnmpi_wtime();
	ncs2nrn_integrate(tstop*(1.+1e-11));
//figure out where to store these
//	impl_->integ_time_ += nrnmpi_wtime() - wt;
//	impl_->integ_time_ -= (npe_ ? (npe_[0].wx_ + npe_[0].ws_) : 0.);
	nrn_spike_exchange(nrn_threads);
	nrn_timeout(0);
//	impl_->wait_time_ += wt_;
//	impl_->send_time_ += wt1_;
	if (npe_) {
//		impl_->wait_time_ += npe_[0].wx_;
//		impl_->send_time_ += npe_[0].ws_;
		npe_[0].wx_ = npe_[0].ws_ = 0.;
	};
//printf("%d netpar_solve exit t=%g tstop=%g mindelay_=%g\n",nrnmpi_myid, t, tstop, mindelay_);
#else // not NRNMPI
	ncs2nrn_integrate(tstop);
#endif
	tstopunset;

        nrnmpi_barrier();
	if ( nrnmpi_myid == 0 ) {
        	printf( " Solver Time : %g\n", nrnmpi_wtime() - time );
    	}
}

static double set_mindelay(double maxdelay) {
	double mindelay = maxdelay;
	last_maxstep_arg_ = maxdelay;
			
    // if all==1 then minimum delay of all NetCon no matter the source.
   // except if src in same thread as NetCon
   int all = (nrn_use_selfqueue_ || nrn_nthread > 1);
   // minumum delay of all NetCon having an InputPreSyn source
   for (int ith = 0; ith < nrn_nthread; ++ith) {
       NrnThread* nt = nrn_threads + ith;
       for (int i = 0; i < nt->n_netcon; ++i) {
           NetCon& nc = nt->netcons[i];
           int chk = 0; // ignore nc.delay_
           if (nc.src_ && nc.src_->type() == InputPreSynType) {
               chk = 1;
           }else if (all) {
               chk = 1;
               // but ignore if src in same thread as NetCon
               if (nc.src_ && nc.src_->type() == PreSynType
                   && ((PreSyn*)nc.src_)->nt_ == nt) {
                   chk = 0;
               }
           }
           if (chk && nc.delay_ < mindelay) {
                mindelay = nc.delay_;
           } 
		}
	}

#if NRNMPI
	if (nrnmpi_use) {active_ = 1;}
	if (use_compress_) {
		if (mindelay/dt > 255) {
			mindelay = 255*dt;
		}
	}

//printf("%d netpar_mindelay local %g now calling nrnmpi_mindelay\n", nrnmpi_myid, mindelay);
//	double st = time();
	mindelay_ = nrnmpi_mindelay(mindelay);
	min_interprocessor_delay_ = mindelay_;
//	add_wait_time(st);
//printf("%d local min=%g  global min=%g\n", nrnmpi_myid, mindelay, mindelay_);
	if (mindelay_ < 1e-9 && nrn_use_selfqueue_) {
		nrn_use_selfqueue_ = 0;
		double od = mindelay_;
		mindelay = set_mindelay(maxdelay);
		if (nrnmpi_myid == 0) {
printf("Notice: The global minimum NetCon delay is %g, so turned off the cvode.queue_mode\n", od);
printf("   use_self_queue option. The interprocessor minimum NetCon delay is %g\n", mindelay);
		}
	}
	errno = 0;
	return mindelay;
#else
	mindelay_ = mindelay;
	min_interprocessor_delay_ = mindelay_;
	return mindelay;
#endif //NRNMPI
}

double BBS_netpar_mindelay(double maxdelay) {
	double tt = set_mindelay(maxdelay);
	return tt;
}

void BBS_netpar_spanning_statistics(int* nsend, int* nsendmax, int* nrecv, int* nrecv_useful) {
#if NRNMPI
	*nsend = nsend_;
	*nsendmax = nsendmax_;
	*nrecv = nrecv_;
	*nrecv_useful = nrecv_useful_;
#endif
}

/*  08-Nov-2010
The workhorse for spike exchange on up to 10K machines is MPI_Allgather
but as the number of machines becomes far greater than the fanout per
cell we have been exploring a class of exchange methods called multisend
where the spikes only go to those machines that need them and there is
overlap between communication and computation.  The numer of variants of
multisend has grown so that some method selection function is needed
that makes sense. 

The situation that needs to be captured by xchng_meth is

Allgather
multisend implemented as MPI_ISend
multisend DCMF (only for Blue Gene/P)
multisend record_replay (only for Blue Gene/P with recordreplay_v1r4m2.patch)

n_bgp_interval 1 or 2 per minimum interprocessor NetCon delay
 that concept valid for all methods

Note that Allgather allows spike compression and an allgather spike buffer
 with size chosen at setup time.  All methods allow bin queueing.

All the multisend methods should allow two phase multisend.

Note that, in principle, MPI_ISend allows the source to send the index   
 of the target PreSyn to avoid a hash table lookup (even with a two phase
 variant)

Not all variation are useful. e.g. it is pointless to combine Allgather and
n_bgp_interval=2.
RecordReplay should be best on the BG/P. The whole point is to make the
spike transfer initiation as lowcost as possible since that is what causes
most load imbalance. I.e. since 10K more spikes arrive than are sent, spikes
received per processor per interval are much more statistically
balanced than spikes sent per processor per interval. And presently
DCMF multisend injects 10000 messages per spike into the network which
is quite expensive. record replay avoids this overhead and the idea of
two phase multisend distributes the injection

See case 8 of nrn_bgp_receive_time for the xchng_meth properties
*/

int nrnmpi_spike_compress(int nspike, bool gid_compress, int xchng_meth) {
#if NRNMPI
	if (nrnmpi_numprocs < 2) { return 0; }
#if BGP_INTERVAL == 2
	n_bgp_interval = (xchng_meth & 4) ? 2 : 1;
#endif
	assert(xchng_meth == 0);
	if (nspike >= 0) {
		ag_send_nspike_ = 0;
		if (spfixout_) { free(spfixout_); spfixout_ = 0; }
		if (spfixin_) { free(spfixin_); spfixin_ = 0; }
		if (spfixin_ovfl_) { free(spfixin_ovfl_); spfixin_ovfl_ = 0; }
        localmaps.clear();
	}
	if (nspike == 0) { // turn off
		use_compress_ = false;
		nrn_use_localgid_ = false;
	}else if (nspike > 0) { // turn on
		use_compress_ = true;
		ag_send_nspike_ = nspike;
		nrn_use_localgid_ = false;
		if (gid_compress) {
			// we can only do this after everything is set up
			mk_localgid_rep();
			if (!nrn_use_localgid_ && nrnmpi_myid == 0) {
printf("Notice: gid compression did not succeed. Probably more than 255 cells on one cpu.\n");
			}
		}
		if (!nrn_use_localgid_) {
			localgid_size_ = sizeof(unsigned int);
		}
		ag_send_size_ = 2 + ag_send_nspike_*(1 + localgid_size_);
		spfixout_capacity_ = ag_send_size_ + 50*(1 + localgid_size_);
		spfixout_ = (unsigned char*)emalloc(spfixout_capacity_);
		spfixin_ = (unsigned char*)emalloc(nrnmpi_numprocs*ag_send_size_);
		ovfl_capacity_ = 100;
		spfixin_ovfl_ = (unsigned char*)emalloc(ovfl_capacity_*(1 + localgid_size_));
	}
	return ag_send_nspike_;
#else
	return 0;
#endif
}


/// Approximate count of number of bytes for the gid2out map
size_t output_presyn_size(void) {
  if (gid2out.empty()) { return 0; }
  size_t nbyte = sizeof(gid2out) + sizeof(int)*gid2out.size() + sizeof(PreSyn*)*gid2out.size();
#ifdef DEBUG
  printf(" gid2out table bytes=~%ld size=%d\n", nbyte, gid2out.size());
#endif
  return nbyte;
}

size_t input_presyn_size(void) {
  if (gid2in.empty()) { return 0; }
  size_t nbyte = sizeof(gid2in) + sizeof(int)*gid2in.size() + sizeof(InputPreSyn*)*gid2in.size();
#ifdef DEBUG
  printf(" gid2in table bytes=~%ld size=%d\n", nbyte, gid2in->size());
#endif
  return nbyte;
}

