#include "hn.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "packet.h"
#include "crypt.h"
#include "util.h"
#include "chan.h"

hn_t hn_free(hn_t hn)
{
  if(!hn) return NULL;
  if(hn->chans) xht_free(hn->chans);
  if(hn->c) crypt_free(hn->c);
  if(hn->parts) packet_free(hn->parts);
  if(hn->paths) free(hn->paths);
  free(hn);
  return NULL;
}

hn_t hn_get(xht_t index, unsigned char *bin)
{
  hn_t hn;
  unsigned char hex[65];
  
  if(!bin) return NULL;
  util_hex(bin,32,hex);
  hn = xht_get(index, (const char*)hex);
  if(hn) return hn;

  // init new hashname container
  if(!(hn = malloc(sizeof (struct hn_struct)))) return NULL;
  memset(hn,0,sizeof (struct hn_struct));
  memcpy(hn->hashname, bin, 32);
  memcpy(hn->hexname, hex, 65);
  xht_set(index, (const char*)hn->hexname, (void*)hn);
  if(!(hn->paths = malloc(sizeof (path_t)))) return hn_free(hn);
  hn->paths[0] = NULL;
  return hn;
}

hn_t hn_gethex(xht_t index, char *hex)
{
  hn_t hn;
  unsigned char bin[32];
  if(!hex || strlen(hex) < 64) return NULL;
  if((hn = xht_get(index,hex))) return hn;
  util_unhex((unsigned char*)hex,64,bin);
  return hn_get(index,bin);
}

int csidcmp(void *arg, const void *a, const void *b)
{
  if(*(char*)a == *(char*)b) return *(char*)(a+1) - *(char*)(b+1);
  return *(char*)a - *(char*)b;
}

hn_t hn_getparts(xht_t index, packet_t p)
{
  char *part, csid, csids[16], hex[3]; // max parts of 8
  int i,ids,ri,len;
  unsigned char *rollup, hnbin[32];
  char best = 0;
  hn_t hn;

  if(!p) return NULL;
  hex[2] = 0;

  for(ids=i=0;ids<8 && p->js[i];i+=4)
  {
    if(p->js[i+1] != 2) continue; // csid must be 2 char only
    memcpy(hex,p->json+p->js[i],2);
    memcpy(csids+(ids*2),hex,2);
    util_unhex((unsigned char*)hex,2,(unsigned char*)&csid);
    if(csid > best && xht_get(index,hex)) best = csid; // matches if we have the same csid in index (for our own keys)
    ids++;
  }
  
  if(!best) return NULL; // we must match at least one
  util_sort(csids,ids,2,csidcmp,NULL);

  rollup = NULL;
  ri = 0;
  for(i=0;i<ids;i++)
  {
    len = 2;
    if(!(rollup = util_reallocf(rollup,ri+len))) return NULL;
    memcpy(rollup+ri,csids+(i*2),len);
    crypt_hash(rollup,ri+len,hnbin);
    ri = 32;
    if(!(rollup = util_reallocf(rollup,ri))) return NULL;
    memcpy(rollup,hnbin,ri);

    memcpy(hex,csids+(i*2),2);
    part = packet_get_str(p, hex);
    if(!part) continue; // garbage safety
    len = strlen(part);
    if(!(rollup = util_reallocf(rollup,ri+len))) return NULL;
    memcpy(rollup+ri,part,len);
    crypt_hash(rollup,ri+len,hnbin);
    memcpy(rollup,hnbin,32);
  }
  memcpy(hnbin,rollup,32);
  free(rollup);
  hn = hn_get(index, hnbin);
  if(!hn) return NULL;

  if(!hn->parts) hn->parts = p;
  else packet_free(p);
  
  hn->csid = best;
  util_hex((unsigned char*)&best,1,(unsigned char*)hn->hexid);

  return hn;
}

hn_t hn_frompacket(xht_t index, packet_t p)
{
  hn_t hn = NULL;
  if(!p) return NULL;
  
  // get/gen the hashname
  hn = hn_getparts(index, packet_get_packet(p, "from"));
  if(!hn) return NULL;

  // load key from packet body
  if(!hn->c && p->body)
  {
    hn->c = crypt_new(hn->csid, p->body, p->body_len);
    if(!hn->c) return NULL;
  }
  return hn;
}

// derive a hn from json seed or connect format
hn_t hn_fromjson(xht_t index, packet_t p)
{
  char *key;
  hn_t hn = NULL;
  packet_t pp, next;
  path_t path;

  if(!p) return NULL;
  
  // get/gen the hashname
  pp = packet_get_packet(p,"from");
  if(!pp) pp = packet_get_packet(p,"parts");
  hn = hn_getparts(index, pp); // frees pp
  if(!hn) return NULL;

  // if any paths are stored, associte them
  pp = packet_get_packets(p, "paths");
  while(pp)
  {
    path = hn_path(hn, path_parse((char*)pp->json, pp->json_len), 0);
    next = pp->next;
    packet_free(pp);
    pp = next;
  }

  // already have crypto
  if(hn->c) return hn;

  if(p->body_len)
  {
    hn->c = crypt_new(hn->csid, p->body, p->body_len);
  }else{
    pp = packet_get_packet(p, "keys");
    key = packet_get_str(pp,hn->hexid);
    if(key) hn->c = crypt_new(hn->csid, (unsigned char*)key, strlen(key));
    packet_free(pp);
  }
  
  return (hn->c) ? hn : NULL;  
}

path_t hn_path(hn_t hn, path_t p, int valid)
{
  path_t ret = NULL;
  int i;

  if(!p) return NULL;

  // find existing matching path
  for(i=0;hn->paths[i];i++)
  {
    if(path_match(hn->paths[i], p)) ret = hn->paths[i];
  }
  if(!ret && (ret = path_copy(p)))
  {
    // add new path, i is the end of the list from above
    if(!(hn->paths = util_reallocf(hn->paths, (i+2) * (sizeof (path_t))))) return NULL;
    hn->paths[i] = ret;
    hn->paths[i+1] = 0; // null term
  }

  // update state tracking
  if(ret && valid)
  {
    hn->last = ret;
    ret->tin = platform_seconds();    
  }

  return ret;
}

unsigned char hn_distance(hn_t a, hn_t b)
{
  return 0;
}

