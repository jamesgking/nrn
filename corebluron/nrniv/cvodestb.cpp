#include "corebluron/nrnconf.h"
#include "corebluron/nrnoc/multicore.h"
// solver CVode stub to allow cvode as dll for mswindows version.

#include "corebluron/nrniv/netcvode.h"
#include "corebluron/nrniv/vrecitem.h"

extern "C" {
bool at_time(NrnThread*, double);
#define nt_t nrn_threads->_t
#define nt_dt nrn_threads->_dt

extern NetCvode* net_cvode_instance;
void nrn_solver_prepare(void);

// for fixed step thread
void deliver_net_events(NrnThread* nt) {
	(void)nt;
	if (net_cvode_instance) {
		net_cvode_instance->check_thresh(nt);
		net_cvode_instance->deliver_net_events(nt);
	}
}

// handle events during finitialize()
void nrn_deliver_events(NrnThread* nt) {
	double tsav = nt->_t;
	if (net_cvode_instance) {
		net_cvode_instance->deliver_events(tsav, nt);
	}
	nt->_t = tsav;
}

void clear_event_queue() {
	if (net_cvode_instance) {
		net_cvode_instance->clear_events();
	}
}

void init_net_events() {
	if (net_cvode_instance) {
		net_cvode_instance->init_events();
	}
}

void nrn_record_init() {
	return;
	if (net_cvode_instance) {
//		net_cvode_instance->record_init();
	}
}

void nrn_play_init() {
    for (int ith = 0; ith < nrn_nthread; ++ith) {
	NrnThread* nt = nrn_threads + ith;
	for (int i=0; i < nt->n_vecplay; ++i) {
		((PlayRecord*)nt->_vecplay[i])->play_init();
	}
    }
}

void fixed_play_continuous(NrnThread* nt) {
	for (int i=0; i < nt->n_vecplay; ++i) {
		((PlayRecord*)nt->_vecplay[i])->continuous(nt->_t);
	}
}

void fixed_record_continuous(NrnThread* nt) {
	(void)nt; return;
	if (net_cvode_instance) {
//		net_cvode_instance->fixed_record_continuous(nt);
	}
}

void nrn_random_play(NrnThread* nt) {
	(void)nt;
	return;
}

void nrn_solver_prepare() {
	if (net_cvode_instance) {
		net_cvode_instance->solver_prepare();
	}
}

bool at_time(NrnThread* nt, double te) {
	double x = te - 1e-11;
	if (x <= nt->_t && x > (nt->_t - nt->_dt)) {
		return 1;
	}
	return 0;
}

}
