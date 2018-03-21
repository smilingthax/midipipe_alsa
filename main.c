#include <stdio.h>
//#include <getopt.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

// TODO? <stdbool.h> ?

// TODO? snd_seq_t  from outside?
static void list_ports(int only) // {{{
{
  // assert( (only>=0)&&(only<=2) );
  snd_seq_t *seq;
  int ret=snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0);
  if (ret) {
    fprintf(stderr, "Alsa open error %d: %s\n", ret, snd_strerror(ret));
    return;
  }

  int self=snd_seq_client_id(seq);

  snd_seq_client_info_t *cinfo;
  snd_seq_port_info_t *pinfo;

  snd_seq_client_info_alloca(&cinfo);
  snd_seq_port_info_alloca(&pinfo);

  snd_seq_client_info_set_client(cinfo, -1);
  while ((ret=snd_seq_query_next_client(seq, cinfo))==0) {
    const int client=snd_seq_client_info_get_client(cinfo);
    if (client==self) {
      continue; // note: we don't have a port, at this point
    }

    snd_seq_port_info_set_client(pinfo, client);
    snd_seq_port_info_set_port(pinfo, -1);
    while ((ret=snd_seq_query_next_port(seq, pinfo))==0) {
      const int capa=snd_seq_port_info_get_capability(pinfo);
      if (capa&SND_SEQ_PORT_CAP_NO_EXPORT) {
        continue;
      }
      if ( (only!=2)&&((capa&SND_SEQ_PORT_CAP_READ)==0)&&((capa&SND_SEQ_PORT_CAP_SUBS_READ)==0) ) {
        continue;
      }
      if ( (only!=1)&&((capa&SND_SEQ_PORT_CAP_WRITE)==0)&&((capa&SND_SEQ_PORT_CAP_SUBS_WRITE)==0) ) {
        continue;
      }

      printf("%d:%d %s\n", client, snd_seq_port_info_get_port(pinfo), snd_seq_port_info_get_name(pinfo));
    }
  }

  snd_seq_close(seq);
}
// }}}

#include <signal.h>
volatile sig_atomic_t done=0;

static void _sigint(int signum)
{
  fprintf(stderr, "Got SIGINT. Exiting...\n");
  done=1;
}

  // TODO? distinguish separators (e.g. '\r\n \t') from invalid chars?  - or: warn, when invalid
static int one_hex(unsigned char ch) // {{{
{
  if ( (ch>='0')&&(ch<='9') ) {
    return ch-'0';
  } else if ( (ch>='a')&&(ch<='f') ) {
    return ch-'a'+10;
  } else if ( (ch>='A')&&(ch<='F') ) {
    return ch-'A'+10;
//  } else if ( (ch!=' ')&&(ch!='\t')&&(ch!='\r')&&(ch!='\n') ) {  ...warn...; return -1; }
  } else {
    return -1;
  }
}
// }}}

// initial state should be -1
static int decode_hex_inplace(unsigned char *buf, int len, int *state) // {{{
{
  // assert(state);
  unsigned const char *in=buf,*end=buf+len;
  unsigned char *out=buf;
  int nib=*state;
  while (in<end) {
    const int val=one_hex(*in++);
    if (nib>=0) {
      if (val<0) {
        *out++=(nib&0xf);
      } else {
        *out++=((nib&0xf)<<4) | (val);
      }
      nib=-1;
    } else {
      nib=val;
    }
  }
  *state=nib;
  return out-buf;
}
// }}}

static void hexdump(const unsigned char *buf, int len, int not_last) // {{{
{
  int i;
  if (!len) {
    return;
  }
  printf("%02X", buf[0]);
  for (i=1; i<len; i++) {
    printf(" %02X", buf[i]);
  }
  if (!not_last) {
    printf("\n");
  }
}
// }}}

struct send_data_s {
  snd_seq_t *sh;
  snd_seq_addr_t *own;
};

static void send_one(struct send_data_s *sds, snd_seq_event_t *ev) // {{{
{
  snd_seq_ev_set_source(ev, sds->own->port);
  snd_seq_ev_set_subs(ev);
  snd_seq_ev_set_direct(ev);

  snd_seq_event_output(sds->sh, ev);
  snd_seq_drain_output(sds->sh);
}
// }}}

void process_midi(snd_midi_event_t *decoder, int *active_sysex, snd_seq_event_t *ev, int opt_bin) // {{{
{
  // assert(decoder);
  // assert(active_sysex);
  int size=12;
  switch (ev->type) {
  // channel voice
  case SND_SEQ_EVENT_NOTEOFF:    // 0x8.
  case SND_SEQ_EVENT_NOTEON:     // 0x9.
  case SND_SEQ_EVENT_KEYPRESS:   // 0xa. (per-key aftertouch)
  case SND_SEQ_EVENT_CONTROLLER: // 0xb.
  case SND_SEQ_EVENT_PGMCHANGE:  // 0xc.
  case SND_SEQ_EVENT_CHANPRESS:  // 0xd. (channel aftertouch)
  case SND_SEQ_EVENT_PITCHBEND:  // 0xe.
    break;
  // double-message controllers
  case SND_SEQ_EVENT_CONTROL14:
  case SND_SEQ_EVENT_NONREGPARAM:
  case SND_SEQ_EVENT_REGPARAM:
    break;

  case SND_SEQ_EVENT_SYSEX:
    size+=ev->data.ext.len;
    break;

  // system common
  case SND_SEQ_EVENT_QFRAME:   // 0xf1
  case SND_SEQ_EVENT_SONGPOS:  // 0xf2
  case SND_SEQ_EVENT_SONGSEL:  // 0xf3
  // ? 0xf4, 0xf5
  case SND_SEQ_EVENT_TUNE_REQUEST: // 0xf6
    break;

  // system realtime
  case SND_SEQ_EVENT_CLOCK:    // 0xf8
  // ? 0xf9
  case SND_SEQ_EVENT_START:    // 0xfa
  case SND_SEQ_EVENT_CONTINUE: // 0xfb
  case SND_SEQ_EVENT_STOP:     // 0xfc
  // ? 0xfd
  case SND_SEQ_EVENT_SENSING:  // 0xfe
  case SND_SEQ_EVENT_RESET:    // 0xff
    return; // TODO?! if (!opt_realtime) return;
    break;

  default:
    fprintf(stderr, "Got unsupported event: %d\n", ev->type);
    break;
  }

  unsigned char buf[size];
  int len=snd_midi_event_decode(decoder, buf, sizeof(buf), ev);
  if (len<0) {
    fprintf(stderr, "Warning: decode error %d: %s\n", len, snd_strerror(len));
  } else if (len>0) {
    if (opt_bin) {
      write(1, buf, len); // (note: we want immediate flush)
    } else {
      if (*active_sysex) {
        printf(" ");
      } else if (buf[0]==0xf0) {
        *active_sysex=1;
      }
      if (buf[len-1]==0xf7) {
        *active_sysex=0;
      }
      hexdump(buf, len, *active_sysex);
    }
  }
}
// }}}

void process_stdin(snd_midi_event_t *encoder, const unsigned char *buf, int len, struct send_data_s *sds) // {{{
{
  // assert(encoder);
  snd_seq_event_t ev;
  snd_seq_ev_clear(&ev);
  while (1) {
    int res=snd_midi_event_encode(encoder, buf, len, &ev);
    if (res<0) {
      fprintf(stderr, "Warning: encode error %d: %s\n", res, snd_strerror(res));
      return;
    }
    if (res==0) {
      break;
    }
    if (ev.type!=SND_SEQ_EVENT_NONE) {
      send_one(sds, &ev);
    }
    if (res>=len) {
      break;
    }
    buf+=res;
    len-=res;
  }
}
// }}}

int main(int argc, char **argv)
{
  int opt_list=0, usage=0, opt_only=0, opt_bin=0;
  const char *rport=NULL;

  int c;
  while ((c=getopt(argc, argv, "hliob"))!=-1) {
    switch(c) {
    case 'l':
      opt_list=1;
      break;

    case 'i':
      opt_only|=1;
      break;
    case 'o':
      opt_only|=2;
      break;

    case 'b':
      opt_bin=1;
      break;

    default:
    case 'h':
      usage=1;
      break;
    }
  }
  if (opt_only==3) {
    fprintf(stderr, "Error: Only one of -i, -o may be given\n");
    usage=1;
  }
  if ( (!opt_list)&&(!usage) ) {
    if (optind<argc) {
      rport=argv[optind];
    } else {
      usage=1;
    }
  }
  if (usage) {
    printf("Usage: %s [-l] [-h] [-i|-o] [-b] [device]\n"
           "  -l: List devices (can be combined with -i|-o)\n"
           "  -i: Input from device only\n"
           "  -o: Output to device only\n"
           "  -b: Binary in-/output (instead of Hex)\n"
           "  -h: Show Usage\n",
           argv[0]);
    return 1;
  } else if (opt_list) {
    list_ports(opt_only);
    return 0;
  } else {
    // assert(optind<argc);
  }

#if 0
  signal(SIGINT, _sigint);
#else
  struct sigaction sact;
  sigemptyset(&sact.sa_mask);
  sact.sa_handler=_sigint;
  sact.sa_flags=SA_RESTART;
  sigaction(SIGINT, &sact, NULL); // TODO? <0: error ...
#endif

  snd_seq_t *sh;

  int ret;
  if (opt_only==1) { // only input
    ret=snd_seq_open(&sh, "default", SND_SEQ_OPEN_INPUT, 0);
  } else if (opt_only==2) { // only output
    ret=snd_seq_open(&sh, "default", SND_SEQ_OPEN_OUTPUT, 0);
  } else { // duplex
    ret=snd_seq_open(&sh, "default", SND_SEQ_OPEN_DUPLEX, 0);
  }
  if (ret<0) {
    fprintf(stderr, "Alsa open error %d: %s\n", ret, snd_strerror(ret));
    return 2;
  }

  ret=snd_seq_set_client_name(sh, "midipipe");
  if (ret<0) {
    fprintf(stderr, "Alsa set client name error %d: %s\n", ret, snd_strerror(ret));
    snd_seq_close(sh);
    return 2;
  }

  snd_seq_addr_t own;
  ret=snd_seq_client_id(sh);
  if (ret<0) {
    fprintf(stderr, "Alsa get client id failed %d: %s\n", ret, snd_strerror(ret));
    snd_seq_close(sh);
    return 2;
  }
  own.client=ret;

  const int capa=SND_SEQ_PORT_CAP_SUBS_READ|SND_SEQ_PORT_CAP_SUBS_WRITE;
  const int type=SND_SEQ_PORT_TYPE_APPLICATION |SND_SEQ_PORT_TYPE_MIDI_GENERIC;
  if (opt_only==1) {
    ret=snd_seq_create_simple_port(sh, "Inport", capa|SND_SEQ_PORT_CAP_WRITE, type);
  } else if (opt_only==2) {
    ret=snd_seq_create_simple_port(sh, "Outport", capa|SND_SEQ_PORT_CAP_READ, type);
  } else {
    ret=snd_seq_create_simple_port(sh, "IOport", capa|SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_WRITE, type);
  }
  if (ret<0) {
    fprintf(stderr, "Alsa create port error %d: %s\n", ret, snd_strerror(ret));
    snd_seq_close(sh);
    return 2;
  }
  own.port=ret;

  // parse + connect to rport
  snd_seq_addr_t other;
  ret=snd_seq_parse_address(sh, &other, rport);
  if (ret<0) {
    fprintf(stderr, "Alsa address parsing of \"%s\" failed %d: %s\n", rport, ret, snd_strerror(ret));
    snd_seq_close(sh);
    return 2;
  }

  snd_seq_port_subscribe_t *subs;
  snd_seq_port_subscribe_alloca(&subs);
  if (opt_only==1) {
    snd_seq_port_subscribe_set_sender(subs, &other);
    snd_seq_port_subscribe_set_dest(subs, &own);
    ret=snd_seq_subscribe_port(sh, subs);
  } else if (opt_only==2) {
    snd_seq_port_subscribe_set_sender(subs, &own);
    snd_seq_port_subscribe_set_dest(subs, &other);
    ret=snd_seq_subscribe_port(sh, subs);
  } else {
    snd_seq_port_subscribe_set_sender(subs, &other);
    snd_seq_port_subscribe_set_dest(subs, &own);
    ret=snd_seq_subscribe_port(sh, subs);

    if (ret==0) {
      snd_seq_port_subscribe_set_sender(subs, &own);
      snd_seq_port_subscribe_set_dest(subs, &other);
      ret=snd_seq_subscribe_port(sh, subs);
    }
  }
  if (ret<0) {
    fprintf(stderr, "Alsa connect error %d: %s\n", ret, snd_strerror(ret));
    snd_seq_close(sh);
    return 3;
  }

  // setup poll descriptors
  int npfd=1;
  if (opt_only!=2) {
    npfd+=snd_seq_poll_descriptors_count(sh, POLLIN);
  }

  struct pollfd *pfds=alloca(sizeof(struct pollfd)*npfd);
  pfds[0].fd=0; // stdin
  pfds[0].events=POLLIN;
  if (opt_only==1) {
    pfds[0].fd=-1; // disable
  }
  if (opt_only!=2) {
    ret=snd_seq_poll_descriptors(sh, pfds+1, npfd-1, POLLIN);
    assert(ret==npfd-1);
  }

  // setup encoder/decoder
  snd_midi_event_t *encoder=NULL, *decoder=NULL;
  if (opt_only!=2) {
    ret=snd_midi_event_new(0, &decoder);
    if (ret<0) {
      fprintf(stderr, "Alsa new midi event decoder failed %d: %s\n", ret, snd_strerror(ret));
      snd_seq_close(sh);
      return 2;
    }
    snd_midi_event_no_status(decoder, 1); // no running status
  }
  if (opt_only!=1) {
    ret=snd_midi_event_new(256, &encoder);
    if (ret<0) {
      fprintf(stderr, "Alsa new midi event encoder failed %d: %s\n", ret, snd_strerror(ret));
      snd_midi_event_free(decoder);
      snd_seq_close(sh);
      return 2;
    }
  }

  struct send_data_s sds={
    .sh = sh,
    .own = &own
  };

  // main loop
  int stdin_hex=-1, active_sysex=0;
  while (!done) {
    ret=poll(pfds, npfd, -1);
    if (ret<0) {
      fprintf(stderr, "Poll failed %d: %s\n", errno, strerror(errno));
      goto error;
    }
    if (ret>0) {
      // stdin
      if (pfds[0].revents&POLLIN) {
        assert(encoder);
        unsigned char buf[100];
        ret=read(0, buf, sizeof(buf));
        if (ret<0) {
          break;
//          goto error;  ?
        }
        if (opt_bin) {
          process_stdin(encoder, buf, ret, &sds);
        } else {
          int len=decode_hex_inplace(buf, ret, &stdin_hex);
          process_stdin(encoder, buf, len, &sds);
        }
      }

      // alsa
      if (npfd<=1) {
        continue;
      }
      unsigned short revents;
      ret=snd_seq_poll_descriptors_revents(sh, pfds+1, npfd-1, &revents);
      if (ret<0) {
        fprintf(stderr, "Alsa poll error %d: %s\n", ret, snd_strerror(ret));
        goto error;
      }
      if (revents&POLLIN) {
        snd_seq_event_t *ev;
        do {
          ret=snd_seq_event_input(sh, &ev);
          if (ret<0) {
            // ? handle ENOSPC
            fprintf(stderr, "Alsa event input error %d: %s\n", ret, snd_strerror(ret));
            goto error;
          }
          assert(ev);
          process_midi(decoder, &active_sysex, ev, opt_bin);
        } while (snd_seq_event_input_pending(sh, 0)>0);
      }
    }
  }

  snd_midi_event_free(decoder);
  snd_midi_event_free(encoder);
  snd_seq_close(sh);
  return 0;

error:
  snd_midi_event_free(decoder);
  snd_midi_event_free(encoder);
  snd_seq_close(sh);
  return 2;
}
