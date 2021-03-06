#ifndef ext_link_h
#define ext_link_h

#include "xht.h"
#include "chan.h"

void ext_link(chan_t c);

void link_free(switch_t s);

// automatically mesh any links, defaults to 0, set to max mesh
void link_mesh(switch_t s, int max);

// enable acting as a seed, defaults to 0, or set to max links (as a prime#!)
void link_seed(switch_t s, int max);

// create/fetch/maintain a link to this hn, fires note with "link":"up" and "link":"down" change events
chan_t link_hn(switch_t s, hn_t h, packet_t note);

#endif
