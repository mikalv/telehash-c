#include "chan.h"
#include <string.h>
#include <stdlib.h>
#include "util.h"
#include "switch.h"

// flags channel as ended either in or out
void doend(chan_t c)
{
  if(!c || c->ended) return;
  DEBUG_PRINTF("channel ended %d",c->id);
  c->ended = 1;
}

// immediately removes channel, creates/sends packet to app to notify
void doerror(chan_t c, packet_t p, char *err)
{
  if(c->ended) return;
  DEBUG_PRINTF("channel fail %d %s %d %d %d",c->id,err?err:packet_get_str(p,"err"),c->timeout,c->tsent,c->trecv);

  // unregister for any more packets immediately
  xht_set(c->to->chans,(char*)c->hexid,NULL);
  if(c->id > c->to->chanMax) c->to->chanMax = c->id;

  // convenience to generate error packet
  if(!p)
  {
    if(!(p = packet_new())) return;
    packet_set_str(p,"err",err?err:"unknown");
    packet_set_int(p,"c",c->id);
  }

  // send error to app immediately, will doend() when processed
  p->next = c->in;
  c->in = p;
  chan_queue(c);
}

void chan_free(chan_t c)
{
  packet_t p;
  if(!c) return;

  // if there's an arg set, we can't free and must notify channel handler
  if(c->arg) return chan_queue(c);

  DEBUG_PRINTF("channel free %d",c->id);

  // unregister for packets (if registered)
  if(xht_get(c->to->chans,(char*)c->hexid) == (void*)c)
  {
    xht_set(c->to->chans,(char*)c->hexid,NULL);
    // to prevent replay of this old id
    if(c->id > c->to->chanMax) c->to->chanMax = c->id;
  }
  xht_set(c->s->index,(char*)c->uid,NULL);

  // remove references
  chan_dequeue(c);
  if(c->reliable)
  {
    chan_seq_free(c);
    chan_miss_free(c);
  }
  while(c->in)
  {
    DEBUG_PRINTF("unused packets on channel %d",c->id);
    p = c->in;
    c->in = p->next;
    packet_free(p);
  }
  while(c->notes)
  {
    DEBUG_PRINTF("unused notes on channel %d",c->id);
    p = c->notes;
    c->notes = p->next;
    packet_free(p);
  }
  // TODO, if it's the last channel, bucket_rem(c->s->active, c);
  free(c->type);
  free(c);
}

void walkreset(xht_t h, const char *key, void *val, void *arg)
{
  chan_t c = (chan_t)val;
  if(!c) return;
  if(c->opened) return doerror(c,NULL,"reset");
  // flush any waiting packets
  if(c->reliable) chan_miss_resend(c);
  else switch_send(c->s,packet_copy(c->in));
}
void chan_reset(switch_t s, hn_t to)
{
  // fail any existing open channels
  xht_walk(to->chans, &walkreset, NULL);
  // reset max id tracking
  to->chanMax = 0;
}

void walktick(xht_t h, const char *key, void *val, void *arg)
{
  uint32_t last;
  chan_t c = (chan_t)val;

  // latent cleanup/free
  if(c->ended && (c->tfree++) > c->timeout) return chan_free(c);

  // any channel can get tick event
  if(c->tick) c->tick(c);

  // these are administrative/internal channels (to our own hashname)
  if(c->to == c->s->id) return;

  // do any auto-resend packet timers
  if(c->tresend)
  {
    // TODO check packet resend timers, chan_miss_resend or c->in
  }

  // unreliable timeout, use last received unless none, then last sent
  if(!c->reliable)
  {
    last = (c->trecv) ? c->trecv : c->tsent;
    if(c->timeout && (c->s->tick - last) > c->timeout) doerror(c,NULL,"timeout");
  }

}

void chan_tick(switch_t s, hn_t hn)
{
  xht_walk(hn->chans, &walktick, NULL);
}

chan_t chan_reliable(chan_t c, int window)
{
  if(!c || !window || c->tsent || c->trecv || c->reliable) return c;
  c->reliable = window;
  chan_seq_init(c);
  chan_miss_init(c);
  return c;
}

// kind of a macro, just make a reliable channel of this type
chan_t chan_start(switch_t s, char *hn, char *type)
{
  chan_t c;
  if(!s || !hn) return NULL;
  c = chan_new(s, hn_gethex(s->index,hn), type, 0);
  return chan_reliable(c, s->window);
}

chan_t chan_new(switch_t s, hn_t to, char *type, uint32_t id)
{
  chan_t c;
  if(!s || !to || !type) return NULL;

  // use new id if none given
  if(!to->chanOut) to->chanOut = (strncmp(s->id->hexname,to->hexname,64) > 0) ? 1 : 2;
  if(!id)
  {
    id = to->chanOut;
    to->chanOut += 2;
  }else{
    // externally given id can't be ours
    if(id % 2 == to->chanOut % 2) return NULL;
    // must be a newer id
    if(id <= to->chanMax) return NULL;
  }

  DEBUG_PRINTF("channel new %d %s %s",id,type,to->hexname);
  c = malloc(sizeof (struct chan_struct));
  memset(c,0,sizeof (struct chan_struct));
  c->type = malloc(strlen(type)+1);
  memcpy(c->type,type,strlen(type)+1);
  c->s = s;
  c->to = to;
  c->timeout = CHAN_TIMEOUT;
  c->id = id;
  util_hex((unsigned char*)&(s->uid),4,(unsigned char*)c->uid); // switch-wide unique id
  s->uid++;
  util_hex((unsigned char*)&(c->id),4,(unsigned char*)c->hexid);
  // first one on this hashname
  if(!to->chans)
  {
    to->chans = xht_new(17);
    bucket_add(s->active, to);
  }
  xht_set(to->chans,(char*)c->hexid,c);
  xht_set(s->index,(char*)c->uid,c);
  return c;
}

void chan_timeout(chan_t c, uint32_t seconds)
{
  if(!c) return;
  c->timeout = seconds;
}

chan_t chan_in(switch_t s, hn_t from, packet_t p)
{
  chan_t c;
  unsigned long id;
  char hexid[9];
  if(!from || !p) return NULL;

  id = strtol(packet_get_str(p,"c"), NULL, 10);
  util_hex((unsigned char*)&id,4,(unsigned char*)hexid);
  c = xht_get(from->chans, hexid);
  if(c) return c;

  return chan_new(s, from, packet_get_str(p, "type"), id);
}

// get the next incoming note waiting to be handled
packet_t chan_notes(chan_t c)
{
  packet_t note;
  if(!c) return NULL;
  note = c->notes;
  if(note) c->notes = note->next;
  return note;
}

// create a new note tied to this channel
packet_t chan_note(chan_t c, packet_t note)
{
  if(!note) note = packet_new();
  packet_set_str(note,".from",(char*)c->uid);
  return note;
}

// send this note back to the sender
int chan_reply(chan_t c, packet_t note)
{
  char *from;
  if(!c || !(from = packet_get_str(note,".from"))) return -1;
  packet_set_str(note,".to",from);
  packet_set_str(note,".from",(char*)c->uid);
  return switch_note(c->s,note);
}

// create a packet ready to be sent for this channel
packet_t chan_packet(chan_t c)
{
  packet_t p;
  if(!c) return NULL;
  p = c->reliable?chan_seq_packet(c):packet_new();
  if(!p) return NULL;
  p->to = c->to;
  if(!c->tsent && !c->trecv)
  {
    packet_set_str(p,"type",c->type);
  }
  packet_set_int(p,"c",c->id);
  return p;
}

packet_t chan_pop(chan_t c)
{
  packet_t p;
  if(!c) return NULL;
  // this allows errors to be processed immediately
  if((p = c->in))
  {
    c->in = p->next;
    if(!c->in) c->inend = NULL;    
  }
  if(!p && c->reliable) p = chan_seq_pop(c);

  // change state as soon as an err/end is processed
  if(packet_get_str(p,"err") || util_cmp(packet_get_str(p,"end"),"true") == 0) doend(c);
  return p;
}

// add to processing queue
void chan_queue(chan_t c)
{
  chan_t step;
  // add to switch queue
  step = c->s->chans;
  if(c->next || step == c) return;
  while(step && (step = step->next)) if(step == c) return;
  c->next = c->s->chans;
  c->s->chans = c;
}

// remove from processing queue
void chan_dequeue(chan_t c)
{
  chan_t step = c->s->chans;
  if(step == c)
  {
    c->s->chans = c->next;
    c->next = NULL;
    return;
  }
  step = c->s->chans;
  while(step) step = (step->next == c) ? c->next : step->next;
  c->next = NULL;
}

// internal, receives/processes incoming packet
void chan_receive(chan_t c, packet_t p)
{
  if(!c || !p) return;
  DEBUG_PRINTF("channel in %d %d %.*s",c->id,p->body_len,p->json_len,p->json);
  c->trecv = c->s->tick;
  DEBUG_PRINTF("recv %d",c);
  
  // errors are immediate
  if(packet_get_str(p,"err")) return doerror(c,p,NULL);

  if(!c->opened)
  {
    DEBUG_PRINTF("channel opened %d",c->id);
    // remove any cached outgoing packet
    packet_free(c->in);
    c->in = NULL;
    c->opened = 1;
  }

  // TODO, only queue so many packets if we haven't responded ever (to limit ignored unsupported channels)
  if(c->reliable)
  {
    chan_miss_check(c,p);
    if(!chan_seq_receive(c,p)) return; // queued, nothing to do
  }else{
    // add to the end of the raw packet queue
    if(c->inend)
    {
      c->inend->next = p;
      c->inend = p;
      return;
    }
    c->inend = c->in = p;    
  }

  // queue for processing
  chan_queue(c);
}

// convenience
void chan_end(chan_t c, packet_t p)
{
  if(!c) return;
  if(!p && !(p = chan_packet(c))) return;
  packet_set(p,"end","true",4);
  chan_send(c,p);
}

// send errors directly
chan_t chan_fail(chan_t c, char *err)
{
  packet_t p;
  if(!c) return NULL;
  if(!(p = packet_new())) return c;
  doend(c);
  p->to = c->to;
  packet_set_str(p,"err",err?err:"unknown");
  packet_set_int(p,"c",c->id);
  switch_send(c->s,p);
  return c;
}

// smartly send based on what type of channel we are
void chan_send(chan_t c, packet_t p)
{
  if(!p) return;
  if(!c) return (void)packet_free(p);
  DEBUG_PRINTF("channel out %d %.*s",c->id,p->json_len,p->json);
  c->tsent = c->s->tick;
  if(c->reliable && packet_get_str(p,"seq")) p = packet_copy(p); // miss tracks the original p = chan_packet(), a sad hack
  if(packet_get_str(p,"err") || util_cmp(packet_get_str(p,"end"),"true") == 0) doend(c); // track sending error/end
  else if(!c->reliable && !c->opened && !c->in) c->in = packet_copy(p); // if normal unreliable start packet, track for resends
  
  switch_send(c->s,p);
}

// optionally sends reliable channel ack-only if needed
void chan_ack(chan_t c)
{
  packet_t p;
  if(!c || !c->reliable) return;
  p = chan_seq_ack(c,NULL);
  if(!p) return;
  DEBUG_PRINTF("channel ack %d %.*s",c->id,p->json_len,p->json);
  switch_send(c->s,p);
}
