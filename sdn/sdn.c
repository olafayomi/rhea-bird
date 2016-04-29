/*
 *	BIRD -- Binding for SDN controllers
 *
 *	(c) 2013 Sam Russell <sam.h.russell@gmail.com>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

/**
 * DOC: SDN
 *
 * The SDN protocol
 */

#undef LOCAL_DEBUG
#define LOCAL_DEBUG 1

#include <stdlib.h>
#include <unistd.h>
#include <zmq.h>
#include "nest/bird.h"
#include "nest/iface.h"
#include "nest/protocol.h"
#include "nest/route.h"
#include "lib/socket.h"
#include "lib/zeromq.h"
#include "sysdep/unix/unix.h"
#include "lib/resource.h"
#include "lib/lists.h"
#include "lib/timer.h"
#include "lib/string.h"

/* Include header files for socket */
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>


#include "sdn.h"

#define P ((struct sdn_proto *) p)
#define P_CF ((struct sdn_proto_config *)p->cf)

#undef TRACE
#define TRACE(level, msg, args...) do { if (p->debug & level) { log(L_TRACE "%s: " msg, p->name , ## args); } } while(0)

//static struct sdn_interface *new_iface(struct proto *p, struct iface *new, unsigned long flags, struct iface_patt *patt);
static struct sdn_interface *new_iface(struct proto *p, struct iface *new, unsigned long flags, struct iface_patt *patt);
static sock* init_unix_socket(struct proto *p);
static zeromq* init_zeromq(struct proto *p);
static void sdn_route_print_to_sockets(struct proto* p, char* route);
int RheaSockfd;
char  Rheabuffer[256];
int nsent;
/*
 * Input processing
 *
 * This part is responsible for any updates that come from network
 */

/*
 * Output processing
 *
 * This part is responsible for getting packets out to the network.
 */

/* Error logging for TCP client socket */
static void client_error(const char *msg)
{
   log( L_ERR "Unexpected error for RheaFlow client: %s", msg);
}


/* creata a TCP socket and establish a connection to
 * 
 * to RheaFlow.
 */
static int init_rhea_client()
{
    int sockfd, n;
    int portno = 55650;
    char* server_name = "localhost";
    struct sockaddr_in serv_addr;
    struct hostent *server;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0 )
        log( L_ERR "ERROR opening socket to connect to RheaFlow");
    server = gethostbyname(server_name);
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
         (char *)&serv_addr.sin_addr.s_addr,
         server->h_length);
    serv_addr.sin_port = htons(portno);
    /* Initiate connection to the server */
    if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0 )
       log(L_ERR "ERROR connecting to server");

    return sockfd;
}

static void
sdn_tx_err( sock *s, int err )
{
  struct sdn_connection *c = ((struct sdn_interface *)(s->data))->busy;
  struct proto *p = c->proto;
  log( L_ERR "%s: Unexpected error at sdn transmit: %M", p->name, err );
}


/*
 * sdn_tx - send one sdn packet to the network
 */
static void
sdn_tx( sock *s )
{
  log_msg(L_DEBUG "not actually txing but sdn_tx called");
  return;
}


/*
 * sdn_rx - Receive hook: get packet and be awesome
 */
static int
sdn_rx(sock *s, int size)
{
  log_msg( L_DEBUG "Got a packet");
  return 1;
}

static struct sdn_interface*
find_interface(struct proto *p, struct iface *what)
{
  struct sdn_interface *i;

  WALK_LIST (i, P->interfaces)
    if (i->iface == what)
      return i;
  return NULL;
}

/*
 * Interface to BIRD core
 */

static void
sdn_dump_entry( struct sdn_entry *e )
{
  debug( "%I told me %d/%d ago: to %I/%d go via %I, metric %d ",
  e->whotoldme, e->updated-now, e->changed-now, e->n.prefix, e->n.pxlen, e->nexthop, e->metric );
  debug( "\n" );
}

/*
 * sdn_start - initialize instance of sdn
 */
static int
sdn_start(struct proto *p)
{
  //struct sdn_interface *rif;
  struct sdn_zeromq_wrapper *zwrapper;
  DBG( "sdn: starting instance...\n" );

#ifdef LOCAL_DEBUG
  P->magic = SDN_MAGIC;
#endif

  fib_init( &P->rtable, p->pool, sizeof( struct sdn_entry ), 0, NULL );
  init_list( &P->connections );
  init_list( &P->garbage );
  init_list( &P->interfaces );
  init_list( &P->sockets );
  //DBG( "sdn: initialised lists\n" );
  //rif = new_iface(p, NULL, 0, NULL);	/* Initialize dummy interface */
  zwrapper = mb_alloc( p->pool, sizeof( struct sdn_zeromq_wrapper ));
  // we're going to build zmq sockets instead
  //swrapper->skt = init_unix_socket(p);
  zwrapper->skt = init_zeromq(p);
  // Start the RheaFlow client socket 
  RheaSockfd = init_rhea_client();
  // URL tcp://*:5556
  //add_head( &P->interfaces, NODE rif );
  add_head( &P->sockets, NODE zwrapper );
  CHK_MAGIC;

  sdn_init_instance(p);

  DBG( "sdn: ...done\n" );
  return PS_UP;
}

static struct proto *
sdn_init(struct proto_config *cfg)
{
  struct proto *p = proto_new(cfg, sizeof(struct sdn_proto));

  return p;
}

static void
sdn_dump(struct proto *p)
{
  int i;
  node *w;
  struct sdn_interface *rif;

  CHK_MAGIC;
  WALK_LIST( w, P->connections ) {
    struct sdn_connection *n = (void *) w;
    debug( "sdn: connection #%d: %I\n", n->num, n->addr );
  }
  i = 0;
  FIB_WALK( &P->rtable, e ) {
    debug( "sdn: entry #%d: ", i++ );
    sdn_dump_entry( (struct sdn_entry *)e );
  } FIB_WALK_END;
  i = 0;
  WALK_LIST( rif, P->interfaces ) {
    debug( "sdn: interface #%d: %s, %I, busy = %x\n", i++, rif->iface?rif->iface->name:"(dummy)", rif->sock->daddr, rif->busy );
  }
}

static void
sdn_get_route_info(rte *rte, byte *buf, ea_list *attrs)
{
  eattr *metric = ea_find(attrs, EA_SDN_METRIC);
  eattr *tag = ea_find(attrs, EA_SDN_TAG);

  buf += bsprintf(buf, " (%d/%d)", rte->pref, metric ? metric->u.data : 0);
  if (tag && tag->u.data)
    bsprintf(buf, " t%04x", tag->u.data);
}

static void
kill_iface(struct sdn_interface *i)
{
  DBG( "sdn: Interface %s disappeared\n", i->iface->name);
  rfree(i->sock);
  mb_free(i);
}

static void
unix_tx(sock *s)
{
  log_msg(L_DEBUG "sending");
}

static int
zeromq_rx(zeromq *z, int size)
{
  struct proto *p;
  struct sdn_entry *entry;
  char* outbuffer = NULL;
  //char* routestring = "<SDN_DUMP> [%s]\n";
  //char* perroutestring = "{\"prefix\" : \"%I\", \"mask\" : %d, \"via\" : \"%I\"}";
  char* routestring = "<SDN_DUMP> {\"prefix\" : \"%I\", \"mask\" : %d, \"via\" : \"%I\"}";
  char* endstring = "done\n";
  int len = 0;
  //char* addedstring = "<SDN_ANNOUNCE> {\"added\" : [{\"prefix\" : \"%I\", \"mask\" : %d, \"via\" : \"%I\"}] }\n";
  z->rpos = z->rbuf;
  z->rpos[size] = '\0';
  log_msg(L_DEBUG "got packet on socket: <%s>\n", z->rpos);
  p = z->data;
  log_msg(L_DEBUG "");
  FIB_WALK( &P->rtable, e ) {
    entry = (struct sdn_entry*) e;
    len = strlen(routestring) + 33 + 3 + 33;
    outbuffer = xmalloc(len+1);
    outbuffer[len] = '\0';
    bsnprintf(outbuffer, len, routestring, entry->n.prefix, entry->n.pxlen, entry->nexthop);
    //sdn_route_print_to_sockets(p, outbuffer);
    // this should be in zq_write or zq_send, fix later
    log_msg(L_DEBUG "%s\n", outbuffer);
    zmq_send(z->fd, outbuffer, strlen(outbuffer), ZMQ_SNDMORE);
    free(outbuffer);
    //log_msg(L_DEBUG "%I told me %d/%d ago: to %I/%d go via %I, metric %d ",
    //entry->whotoldme, entry->updated-now, entry->changed-now, entry->n.prefix, entry->n.pxlen, entry->nexthop, entry->metric );
  } FIB_WALK_END;
  zmq_send(z->fd, endstring, strlen(endstring), 0);
  // send stuff back to garyland
  //s->tbuf = "gary";
  //sk_send(s, 5);
  return 0;
}

static int
unix_rx(sock *s, int size UNUSED)
{
  struct proto *p;
  struct sdn_entry *entry;
  char* outbuffer = NULL;
  //char* routestring = "<SDN_DUMP> [%s]\n";
  //char* perroutestring = "{\"prefix\" : \"%I\", \"mask\" : %d, \"via\" : \"%I\"}";
  char* routestring = "<SDN_DUMP> {\"prefix\" : \"%I\", \"mask\" : %d, \"via\" : \"%I\"}\n";
  int len = 0;
  //char* addedstring = "<SDN_ANNOUNCE> {\"added\" : [{\"prefix\" : \"%I\", \"mask\" : %d, \"via\" : \"%I\"}] }\n";
  log_msg(L_DEBUG "got packet on socket");
  p = s->data;
  FIB_WALK( &P->rtable, e ) {
    entry = (struct sdn_entry*) e;
    len = strlen(routestring) + 33 + 3 + 33;
    outbuffer = xmalloc(len+1);
    outbuffer[len] = '\0';
    bsnprintf(outbuffer, len, routestring, entry->n.prefix, entry->n.pxlen, entry->nexthop);
    sdn_route_print_to_sockets(p, outbuffer);
    free(outbuffer);
    //log_msg(L_DEBUG "%I told me %d/%d ago: to %I/%d go via %I, metric %d ",
    //entry->whotoldme, entry->updated-now, entry->changed-now, entry->n.prefix, entry->n.pxlen, entry->nexthop, entry->metric );
  } FIB_WALK_END;
  // send stuff back to garyland
  //s->tbuf = "gary";
  //sk_send(s, 5);
  return 0;
}

static void
unix_err(sock *s, int err UNUSED)
{
  log_msg(L_ERR "got error on socket");
}

static int
unix_connect(sock *s, int size UNUSED)
{
  struct proto *p=NULL;
  struct proto_config *pc;
  log_msg(L_DEBUG "someone connected to our socket");
  WALK_LIST(pc, config->protos){
    if (pc->protocol == &proto_sdn && pc->proto){
      log_msg(L_DEBUG "this is the droid we're looking for");
      p = pc->proto;
    }
    log_msg(L_DEBUG "gary");
  }
  if(!p) return 0;
  s->rx_hook = unix_rx;
  s->tx_hook = unix_tx;
  s->type = SK_MAGIC;
  s->err_hook = unix_err;
  s->data = p;
  //s->pool = c->pool;            /* We need to have all the socket buffers allocated in the cli pool */
  //c->rx_pos = c->rx_buf;
  //c->rx_aux = NULL;
  //rmove(s, c->pool);
  // add socket to pool
  CHK_MAGIC;
  //add_head( &P->sockets, NODE s );
  //sk_insert(s);
  return 1;
}

static zeromq*
init_zeromq(struct proto *p)
{
  zeromq *z;
  //char* socketname = (P_CF->unixsocket?P_CF->unixsocket:"/tmp/sdn.sock");
  char* url = "tcp://127.0.0.1:5556";
  log_msg(L_DEBUG "Using URL\n", url);

  z = zq_new(p->pool);
  z->type = ZMQ_REP;
  z->url = xmalloc(strlen(url)+1);
  strcpy(z->url, url);
  z->url[strlen(url)] = '\0';
  //s->rx_hook = unix_connect;
  z->rx_hook = zeromq_rx;
  z->rbsize = 1024;
  // need to set proper rbuf, rbsize etc 
  //if (!s->rbuf && s->rbsize)
      z->rbuf = z->rbuf_alloc = xmalloc(z->rbsize);
      z->rpos = z->rbuf;
      if (!z->tbuf && z->tbsize)
          z->tbuf = z->tbuf_alloc = xmalloc(z->tbsize);
      z->tpos = z->ttx = z->tbuf;
  //}
  z->data = p;

  //unlink(socketname);

  if(zq_open(z) < 0){
    die("Cannot open socket");
  }
  CHK_MAGIC;
  //add_head( &P->sockets, NODE s );
  return z;
}

static sock*
init_unix_socket(struct proto *p)
{
  sock *s;
  char* socketname = (P_CF->unixsocket?P_CF->unixsocket:"/tmp/sdn.sock");
  log_msg(L_DEBUG "Socket in config is %s\n", P_CF->unixsocket);

  s = sk_new(p->pool);
  s->type = SK_UNIX;
  //s->rx_hook = unix_connect;
  s->rx_hook = unix_rx;
  s->rbsize = 1024;
  // need to set proper rbuf, rbsize etc 
  //if (!s->rbuf && s->rbsize)
      s->rbuf = s->rbuf_alloc = xmalloc(s->rbsize);
      s->rpos = s->rbuf;
      if (!s->tbuf && s->tbsize)
          s->tbuf = s->tbuf_alloc = xmalloc(s->tbsize);
      s->tpos = s->ttx = s->tbuf;
  //}
  s->data = p;

  //unlink(socketname);

  if(sk_open_unix_connect(s, socketname) < 0){
    die("Cannot open socket");
  }
  CHK_MAGIC;
  //add_head( &P->sockets, NODE s );
  return s;
}

/**
 * new_iface
 * @p: myself
 * @new: interface to be created or %NULL if we are creating a magic
 * socket. The magic socket is used for listening and also for
 * sending requested responses.
 * @flags: interface flags
 * @patt: pattern this interface matched, used for access to config options
 *
 * Create an interface structure and start listening on the interface.
 */


// new_iface(struct proto *p, struct iface *new, unsigned long flags, struct iface_patt *patt )
//static sock *
//new_listening_iface(ip_addr addr, unsigned port, u32 flags)
//static sock *
static struct sdn_interface*
new_iface(struct proto *p, struct iface *new, unsigned long flags, struct iface_patt *patt )
{
  struct sdn_interface *rif;
  struct sdn_patt *PATT = (struct sdn_patt *) patt;
  rif = mb_allocz(p->pool, sizeof( struct sdn_interface ));
  rif->iface = new;
  rif->proto = p;
  rif->busy = NULL;
  if (PATT) {
    rif->mode = PATT->mode;
    rif->metric = PATT->metric;
    rif->multicast = (!(PATT->mode & IM_BROADCAST)) && (flags & IF_MULTICAST);
    //rif->check_ttl = (PATT->ttl_security == 1);
  }
  /* lookup multicasts over unnumbered links - no: rip is not defined over unnumbered links */

  if (rif->multicast)
    DBG( "Doing multicasts!\n" );

  rif->sock = sk_new( p->pool );
  rif->sock->type = SK_UDP;
  rif->sock->sport = P_CF->port;
  rif->sock->rx_hook = sdn_rx;
  rif->sock->data = rif;
  rif->sock->rbsize = 10240;
  rif->sock->iface = new;		/* Automagically works for dummy interface */
  rif->sock->tbuf = mb_alloc( p->pool, sizeof( struct sdn_packet ));
  rif->sock->tx_hook = sdn_tx;
  rif->sock->err_hook = sdn_tx_err;
  rif->sock->daddr = IPA_NONE;
  rif->sock->dport = P_CF->port;
  if (new)
    {
      rif->sock->tos = PATT->tx_tos;
      rif->sock->priority = PATT->tx_priority;
      rif->sock->ttl = PATT->ttl_security ? 255 : 1;
      // rif->sock->flags = SKF_LADDR_RX | (rif->check_ttl ? SKF_TTL_RX : 0);
    }

  if (new) {
    if (new->addr->flags & IA_PEER)
      log( L_WARN "%s: sdn is not defined over unnumbered links", p->name );
    if (rif->multicast) {
#ifndef IPV6
      rif->sock->daddr = ipa_from_u32(0xe0000009);
#else
      rif->sock->daddr = ipa_build(0xff020000, 0, 0, 9);
#endif
    } else {
      rif->sock->daddr = new->addr->brd;
    }
  }

  if (!ipa_nonzero(rif->sock->daddr)) {
    if (rif->iface)
      log( L_WARN "%s: interface %s is too strange for me", p->name, rif->iface->name );
  } else {

    if (sk_open(rif->sock) < 0)
      goto err;

    if (rif->multicast)
      {
	if (sk_setup_multicast(rif->sock) < 0)
	  goto err;
	if (sk_join_group(rif->sock, rif->sock->daddr) < 0)
	  goto err;
      }
    else
      {
	if (sk_setup_broadcast(rif->sock) < 0)
	  goto err;
      }
  }

  TRACE(D_EVENTS, "Listening on %s, port %d, mode %s (%I)", rif->iface ? rif->iface->name : "(dummy)", P_CF->port, rif->multicast ? "multicast" : "broadcast", rif->sock->daddr );
  
  return rif;

 err:
  sk_log_error(rif->sock, p->name);
  log(L_ERR "%s: Cannot open socket for %s", p->name, rif->iface ? rif->iface->name : "(dummy)" );
  if (rif->iface) {
    rfree(rif->sock);
    mb_free(rif);
    return NULL;
  }
  /* On dummy, we just return non-working socket, so that user gets error every time anyone requests table */
  return rif;

}

/*static struct sdn_interface *
new_iface(struct proto *p, struct iface *new, unsigned long flags, struct iface_patt *patt )
{
  struct sdn_interface *rif;
  struct sdn_patt *PATT = (struct sdn_patt *) patt;

  rif = mb_allocz(p->pool, sizeof( struct sdn_interface ));
  rif->iface = new;
  rif->proto = p;
  rif->busy = NULL;
  if (PATT) {
    rif->mode = PATT->mode;
    rif->metric = PATT->metric;
    rif->multicast = (!(PATT->mode & IM_BROADCAST)) && (flags & IF_MULTICAST);
  }

  if (rif->multicast)
    DBG( "Doing multicasts!\n" );

  rif->sock = sk_new( p->pool );
  rif->sock->type = SK_UDP;
  rif->sock->sport = P_CF->port;
  rif->sock->rx_hook = sdn_rx;
  rif->sock->data = rif;
  rif->sock->rbsize = 10240;
  rif->sock->iface = new;		// Automagically works for dummy interface
  rif->sock->tbuf = mb_alloc( p->pool, sizeof( struct sdn_packet ));
  rif->sock->tx_hook = sdn_tx;
  rif->sock->err_hook = sdn_tx_err;
  rif->sock->daddr = IPA_NONE;
  rif->sock->dport = P_CF->port;
  if (new)
    {
      rif->sock->ttl = 1;
      rif->sock->tos = IP_PREC_INTERNET_CONTROL;
      rif->sock->flags = SKF_LADDR_RX;
    }

  if (new) {
    if (new->addr->flags & IA_PEER)
      log( L_WARN "%s: sdn is not defined over unnumbered links", p->name );
    rif->sock->saddr = IPA_NONE;
    if (rif->multicast) {
#ifndef IPV6
      rif->sock->daddr = ipa_from_u32(0xe0000009);
#else
      rif->sock->daddr = ipa_build(0xff020000, 0, 0, 9);
#endif
    } else {
      rif->sock->daddr = new->addr->brd;
    }
    rif->sock->daddr = ipa_from_u32(0x00000000);
  }
  if (!ipa_nonzero(rif->sock->daddr)) {
    if (rif->iface)
      log( L_WARN "%s: interface %s is too strange for me", p->name, rif->iface->name );
  } else { 

    if (sk_open(rif->sock) < 0)
      goto err;

    if (rif->multicast)
      {
	if (sk_setup_multicast(rif->sock) < 0)
	  goto err;
	if (sk_join_group(rif->sock, rif->sock->daddr) < 0)
	  goto err;
      }
    else
      {
	if (sk_setup_broadcast(rif->sock) < 0)
	  goto err;
      }
  }

  TRACE(D_EVENTS, "Listening on %s, port %d, mode %s (%I)", rif->iface ? rif->iface->name : "(dummy)", P_CF->port, rif->multicast ? "multicast" : "broadcast", rif->sock->daddr );
  
  return rif;

 err:
  sk_log_error(rif->sock, p->name);
  log_msg(L_ERR "Error opening socket on %s", rif->iface);
  log(L_ERR "%s: Cannot open socket for %s", p->name, rif->iface ? rif->iface->name : "(dummy)" );
  if (rif->iface) {
    rfree(rif->sock);
    mb_free(rif);
    return NULL;
  }
  // On dummy, we just return non-working socket, so that user gets error every time anyone requests table
  return rif;
}*/

static void
sdn_real_if_add(struct object_lock *lock)
{
  struct iface *iface = lock->iface;
  struct proto *p = lock->data;
  struct sdn_interface *rif;
  struct iface_patt *k = iface_patt_find(&P_CF->iface_list, iface, iface->addr);

  if (!k)
    bug("This can not happen! It existed few seconds ago!" );
  log_msg(L_DEBUG "adding interface %s", iface->name);
  DBG("adding interface %s\n", iface->name );
  rif = new_iface(p, iface, iface->flags, k);
  if (rif) {
    add_head( &P->interfaces, NODE rif );
    DBG("Adding object lock of %p for %p\n", lock, rif);
    log_msg(L_DEBUG "Adding object lock of %p for %p\n", lock, rif);
    rif->lock = lock;
  } else { rfree(lock); }
}

static void
sdn_if_notify(struct proto *p, unsigned c, struct iface *iface)
{
  DBG( "sdn: if notify\n" );
  log_msg(L_DEBUG "Calling sdn_if_notify for if: %s", iface->name);
  if (iface->flags & IF_IGNORE){
    log_msg(L_DEBUG "Ignoring if %s", iface->name);
    return;
  }
  if (c & IF_CHANGE_DOWN) {
    struct sdn_interface *i;
    log_msg(L_DEBUG "Interface %s going down", iface->name);
    i = find_interface(p, iface);
    if (i) {
      rem_node(NODE i);
      rfree(i->lock);
      kill_iface(i);
    }
  }
  if (c & IF_CHANGE_UP) {
    struct iface_patt *k = iface_patt_find(&P_CF->iface_list, iface, iface->addr);
    struct object_lock *lock;
    struct sdn_patt *PATT = (struct sdn_patt *) k;

    log_msg(L_DEBUG "Interface %s going up", iface->name);

    if (!k) {
      log_msg(L_DEBUG "Not interested in interface %s", iface->name);
      return; /* We are not interested in this interface */
    }

    lock = olock_new( p->pool );
    if (!(PATT->mode & IM_BROADCAST) && (iface->flags & IF_MULTICAST)){
      log_msg(L_DEBUG "multicast and broadcast flags");
#ifndef IPV6
      lock->addr = ipa_from_u32(0xe0000009);
#else
      ip_pton("FF02::9", &lock->addr);
#endif
    }
    else {
      log_msg(L_DEBUG "not multicast/broadcast flags");
      lock->addr = iface->addr->brd;
    }
    lock->port = P_CF->port;
    lock->iface = iface;
    lock->hook = sdn_real_if_add;
    lock->data = p;
    lock->type = OBJLOCK_UDP;
    olock_acquire(lock);
  }
}

static struct ea_list *
sdn_gen_attrs(struct linpool *pool, int metric, u16 tag)
{
  struct ea_list *l = lp_alloc(pool, sizeof(struct ea_list) + 2*sizeof(eattr));

  l->next = NULL;
  l->flags = EALF_SORTED;
  l->count = 2;
  l->attrs[0].id = EA_SDN_TAG;
  l->attrs[0].flags = 0;
  l->attrs[0].type = EAF_TYPE_INT | EAF_TEMP;
  l->attrs[0].u.data = tag;
  l->attrs[1].id = EA_SDN_METRIC;
  l->attrs[1].flags = 0;
  l->attrs[1].type = EAF_TYPE_INT | EAF_TEMP;
  l->attrs[1].u.data = metric;
  return l;
}

static int
sdn_import_control(struct proto *p, struct rte **rt, struct ea_list **attrs, struct linpool *pool)
{
  if ((*rt)->attrs->src->proto == p)	/* My own must not be touched */
    return 1;

  if ((*rt)->attrs->source != RTS_SDN) {
    struct ea_list *new = sdn_gen_attrs(pool, 1, 0);
    new->next = *attrs;
    *attrs = new;
	// say yes for giggles
	return 1;
  }
  return 0;
}

static struct ea_list *
sdn_make_tmp_attrs(struct rte *rt, struct linpool *pool)
{
  return sdn_gen_attrs(pool, rt->u.sdn.metric, rt->u.sdn.tag);
}

static void
sdn_store_tmp_attrs(struct rte *rt, struct ea_list *attrs)
{
  rt->u.sdn.tag = ea_get_int(attrs, EA_SDN_TAG, 0);
  rt->u.sdn.metric = ea_get_int(attrs, EA_SDN_METRIC, 1);
}

static void sdn_route_print_to_sockets(struct proto* p, char* route)
{
  struct sdn_unix_socket_wrapper *swrapper=NULL;
  WALK_LIST(swrapper, P->sockets){
    log_msg(L_DEBUG "printing out to socket %X", swrapper->skt->type);
    swrapper->skt->tbuf=route;
    sk_send(swrapper->skt, strlen(route));
  }
}

static void route_print_to_rhea_socket(int sockfd, char* route)
{
    if (sockfd >= 0) {
       nsent = write(sockfd,route,strlen(route));
       if(nsent < 0) {
         log(L_ERR "Error writing to rhea socket");
       }
       bzero(Rheabuffer, 256);
       nsent = read(sockfd,Rheabuffer,255);
       if (nsent < 0) {
          log(L_ERR "Error reading from rhea socket");
       }
       log_msg(L_DEBUG "Rhea says: %s",Rheabuffer);
     }
    else {
       log(L_ERR "No socket for rhea client");
    }
}
  
static void
sdn_route_mod_str(struct proto *p, struct sdn_entry *e, struct network *net, struct rte *new, struct rte *old)
{
  int varlen=0;
  char* addedstring = "<SDN_ANNOUNCE> {\"added\" : [{\"prefix\" : \"%I\", \"mask\" : %d, \"via\" : \"%I\"}] }\n";
  char* removedstring = "<SDN_ANNOUNCE> {\"removed\" : [{\"prefix\" : \"%I\", \"mask\" : %d, \"via\" : \"%I\"}] }\n";
  char* outbuffer = NULL;
  if(new){
    //log_msg(L_DEBUG "New route: %I", net->n.prefix);
    //log_msg(L_DEBUG "New route: %-1I/%2d ", net->n.prefix, net->n.pxlen);
    //log_msg(L_DEBUG "KF=%02x PF=%02x pref=%d ", net->n.flags, new->pflags, new->pref);
    //if (new->attrs->dest == RTD_ROUTER)
    //  log_msg(" ->%I", new->attrs->gw);
    if (new->attrs->dest == RTD_ROUTER)
    {
      //log_msg(L_DEBUG "<SDN_ANNOUNCE> {\"added\" : [{\"prefix\" : \"%I\", \"mask\" : %d, \"via\" : \"%I\"}] }", net->n.prefix, net->n.pxlen, new->attrs->gw);
      varlen = 33 + 3 + 33 + strlen(addedstring);
      outbuffer = xmalloc(varlen+1);
      outbuffer[varlen] = '\0';
      bsnprintf(outbuffer, varlen, addedstring, net->n.prefix, net->n.pxlen, new->attrs->gw);
      route_print_to_rhea_socket(RheaSockfd, outbuffer);
      log_msg(L_DEBUG "%s", outbuffer);
      //sdn_route_print_to_sockets(p, outbuffer);
      free(outbuffer);
    }
    else
      {
      //log_msg(L_DEBUG "<SDN_ANNOUNCE> {\"added\" : [{\"prefix\" : \"%I\", \"mask\" : %d}] }", net->n.prefix, net->n.pxlen); 
      varlen = 33 + 3 + strlen(addedstring);
      outbuffer = xmalloc(varlen+1);
      outbuffer[varlen] = '\0';
      bsnprintf(outbuffer, varlen, addedstring, net->n.prefix, net->n.pxlen);
      route_print_to_rhea_socket(RheaSockfd, outbuffer);
      log_msg(L_DEBUG "%s", outbuffer);
      //sdn_route_print_to_sockets(p, outbuffer);
      free(outbuffer);
    }
  }
  else{
    //log_msg(L_DEBUG "Removing route: %-1I/%2d ", net->n.prefix, net->n.pxlen);
    //if(old){
    //  log_msg(L_DEBUG "KF=%02x PF=%02x pref=%d ", net->n.flags, old->pflags, old->pref);
    //  if (old->attrs->dest == RTD_ROUTER)
    //    log_msg(" ->%I", old->attrs->gw);
    //}
    if (old && old->attrs->dest == RTD_ROUTER)
    {
      //log_msg(L_DEBUG "<SDN_ANNOUNCE> {\"removed\" : [{\"prefix\" : \"%I\", \"mask\" : %d, \"via\" : \"%I\"}] }", net->n.prefix, net->n.pxlen, old->attrs->gw);
      varlen = 33 + 3 + 33 + strlen(removedstring);
      outbuffer = xmalloc(varlen+1);
      outbuffer[varlen] = '\0';
      bsnprintf(outbuffer, varlen, removedstring, net->n.prefix, net->n.pxlen, old->attrs->gw);
      route_print_to_rhea_socket(RheaSockfd, outbuffer);
      log_msg(L_DEBUG "%s", outbuffer);
      //sdn_route_print_to_sockets(p, outbuffer);
      free(outbuffer);
    }
    else
    {
      // log_msg(L_DEBUG "<SDN_ANNOUNCE> {\"removed\" : [{\"prefix\" : \"%I\", \"mask\" : %d}] }", net->n.prefix, net->n.pxlen);
      varlen = 33 + 3 + strlen(removedstring);
      outbuffer = xmalloc(varlen+1);
      outbuffer[varlen] = '\0';
      bsnprintf(outbuffer, varlen, removedstring, net->n.prefix, net->n.pxlen);
      route_print_to_rhea_socket(RheaSockfd, outbuffer);
      log_msg(L_DEBUG "%s", outbuffer);
      //sdn_route_print_to_sockets(p, outbuffer);
      free(outbuffer);
    }
  }
}

/*
 * sdn_rt_notify - core tells us about new route (possibly our
 * own), so store it into our data structures.
 */
static void
sdn_rt_notify(struct proto *p, struct rtable *table UNUSED, struct network *net,
	      struct rte *new, struct rte *old, struct ea_list *attrs)
{
  CHK_MAGIC;
  struct sdn_entry *e;

  log_msg(L_DEBUG "Calling sdn_rt_notify");
  /*
   * Routes look like this
   * {
   *   "prefix" : "1.1.1.1",
   *   "mask" : 24,
   *   "via" : "192.168.1.1"
   * }
   * Message looks like
   * {
   *   "added": [
   *      {
   *       "prefix" : "1.1.1.0",
   *       "mask" : 24,
   *       "via" : "192.168.1.1"
   *     },
   *      {
   *       "prefix" : "1.1.2.0",
   *       "mask" : 24,
   *       "via" : "192.168.1.1"
   *     }
   *   ]
   *   "removed": [
   *      {
   *       "prefix" : "1.1.3.0",
   *       "mask" : 24,
   *       "via" : "192.168.1.1"
   *     },
   *   ]
   * }
   */
  // get socket details
  
  sdn_route_mod_str(p, e, net, new, old);

  e = fib_find( &P->rtable, &net->n.prefix, net->n.pxlen );
  if (e)
    fib_delete( &P->rtable, e );

  if (new) {
    e = fib_get( &P->rtable, &net->n.prefix, net->n.pxlen );

    e->nexthop = new->attrs->gw;
    e->metric = 0;
    e->whotoldme = IPA_NONE;
    new->u.sdn.entry = e;

    e->tag = ea_get_int(attrs, EA_SDN_TAG, 0);
    e->metric = ea_get_int(attrs, EA_SDN_METRIC, 1);
    if (e->metric > P_CF->infinity)
      e->metric = P_CF->infinity;

    if (new->attrs->src->proto == p)
      e->whotoldme = new->attrs->from;

    if (!e->metric)	/* That's okay: this way user can set his own value for external
			   routes in sdn. */
      e->metric = 5;
    e->updated = e->changed = now;
    e->flags = 0;
  }
}

static int
sdn_rte_same(struct rte *new, struct rte *old)
{
  /* new->attrs == old->attrs always */
  return new->u.sdn.metric == old->u.sdn.metric;
}


static int
sdn_rte_better(struct rte *new, struct rte *old)
{
  struct proto *p = new->attrs->src->proto;

  if (ipa_equal(old->attrs->from, new->attrs->from))
    return 1;

  if (old->u.sdn.metric < new->u.sdn.metric)
    return 0;

  if (old->u.sdn.metric > new->u.sdn.metric)
    return 1;

  if (old->attrs->src->proto == new->attrs->src->proto)		/* This does not make much sense for different protocols */
    if ((old->u.sdn.metric == new->u.sdn.metric) &&
	((now - old->lastmod) > (P_CF->timeout_time / 2)))
      return 1;

  return 0;
}

/*
 * sdn_rte_insert - we maintain linked list of "our" entries in main
 * routing table, so that we can timeout them correctly. sdn_timer()
 * walks the list.
 */
static void
sdn_rte_insert(net *net UNUSED, rte *rte)
{
  struct proto *p = rte->attrs->src->proto;
  CHK_MAGIC;
  DBG( "sdn_rte_insert: %p\n", rte );
  add_head( &P->garbage, &rte->u.sdn.garbage );
}

/*
 * sdn_rte_remove - link list maintenance
 */
static void
sdn_rte_remove(net *net UNUSED, rte *rte)
{
#ifdef LOCAL_DEBUG
  struct proto *p = rte->attrs->src->proto;
  CHK_MAGIC;
  DBG( "sdn_rte_remove: %p\n", rte );
#endif
  rem_node( &rte->u.sdn.garbage );
}

void
sdn_init_instance(struct proto *p)
{
  p->accept_ra_types = RA_ANY;
  p->if_notify = sdn_if_notify;
  p->rt_notify = sdn_rt_notify;
  p->import_control = sdn_import_control;
  p->make_tmp_attrs = sdn_make_tmp_attrs;
  p->store_tmp_attrs = sdn_store_tmp_attrs;
  p->rte_better = sdn_rte_better;
  p->rte_same = sdn_rte_same;
  p->rte_insert = sdn_rte_insert;
  p->rte_remove = sdn_rte_remove;
}

void
sdn_init_config(struct sdn_proto_config *c)
{
  init_list(&c->iface_list);
  c->infinity	= 16;
  c->port	= SDN_PORT;
  c->period	= 30;
  c->garbage_time = 120+180;
  c->timeout_time = 120;
  c->passwords	= NULL;
  c->authtype	= AT_NONE;
}

static int
sdn_get_attr(eattr *a, byte *buf, int buflen UNUSED)
{
  switch (a->id) {
  case EA_SDN_METRIC: bsprintf( buf, "metric: %d", a->u.data ); return GA_FULL;
  case EA_SDN_TAG:    bsprintf( buf, "tag: %d", a->u.data );    return GA_FULL;
  default: return GA_UNKNOWN;
  }
}

static int
sdn_pat_compare(struct sdn_patt *a, struct sdn_patt *b)
{
  return ((a->metric == b->metric) &&
	  (a->mode == b->mode));
}

static int
sdn_reconfigure(struct proto *p, struct proto_config *c)
{
  struct sdn_proto_config *new = (struct sdn_proto_config *) c;
  int generic = sizeof(struct proto_config) + sizeof(list) /* + sizeof(struct password_item *) */;

  if (!iface_patts_equal(&P_CF->iface_list, &new->iface_list, (void *) sdn_pat_compare))
    return 0;
  return !memcmp(((byte *) P_CF) + generic,
                 ((byte *) new) + generic,
                 sizeof(struct sdn_proto_config) - generic);
}

static void
sdn_copy_config(struct proto_config *dest, struct proto_config *src)
{
  /* Shallow copy of everything */
  proto_copy_rest(dest, src, sizeof(struct sdn_proto_config));

  /* We clean up iface_list, ifaces are non-sharable */
  init_list(&((struct sdn_proto_config *) dest)->iface_list);

  /* Copy of passwords is OK, it just will be replaced in dest when used */
}


struct protocol proto_sdn = {
  name: "sdn",
  template: "sdn%d",
  attr_class: EAP_SDN,
  preference: DEF_PREF_SDN,
  get_route_info: sdn_get_route_info,
  get_attr: sdn_get_attr,

  init: sdn_init,
  dump: sdn_dump,
  start: sdn_start,
  reconfigure: sdn_reconfigure,
  copy_config: sdn_copy_config
};
