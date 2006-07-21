#include <stdlib.h>
#include <string.h>

#include <systime.h>
#include <str/str.h>

#include "mailfront.h"
#include "mailrules.h"
#include "smtp.h"

static RESPONSE(too_long, 552, "5.2.3 Sorry, that message exceeds the maximum message length.");
static RESPONSE(hops, 554, "5.6.0 This message is looping, too many hops.");
static RESPONSE(manyrcpt, 550, "5.5.3 Too many recipients");
static RESPONSE(mustauth, 530, "5.7.1 You must authenticate first.");
static RESPONSE(nodomain,554,"5.1.2 Address is missing a domain name");
static RESPONSE(nofqdn,554,"5.1.2 Address does not contain a fully qualified domain name");

const char UNKNOWN[] = "unknown";

static int is_bounce = 0;
static unsigned rcpt_count = 0;

static str received;

const int authenticating = 0;
extern void set_timeout(void);
extern void report_io_bytes(void);

static const char* local_host;
static const char* local_ip;
static const char* remote_host;
static const char* remote_ip;
static str fixup_host;
static str fixup_ip;
static const char* linkproto;
static const char* require_auth;

static const response* check_fqdn(const str* s)
{
  int at;
  int dot;
  if ((at = str_findlast(s, '@')) <= 0)
    return &resp_nodomain;
  if ((dot = str_findlast(s, '.')) < at)
    return &resp_nofqdn;
  return 0;
}

const char* getprotoenv(const char* name)
{
  static str fullname;
  if (!str_copy2s(&fullname, linkproto, name)) return 0;
  return getenv(fullname.s);
}

const response* handle_init(struct session* session)
{
  const char* tmp;
  const response* resp;

  atexit(report_io_bytes);

  set_timeout();

  require_auth = getenv("REQUIRE_AUTH");
  if ((linkproto = getenv("PROTO")) == 0) linkproto = "TCP";
  if ((local_ip = getprotoenv("LOCALIP")) != 0 && *local_ip == 0)
    local_ip = 0;
  if ((remote_ip = getprotoenv("REMOTEIP")) != 0 && *remote_ip == 0)
    remote_ip = 0;
  if ((local_host = getprotoenv("LOCALHOST")) == 0 || *local_host == 0)
    local_host = local_ip;
  if ((remote_host = getprotoenv("REMOTEHOST")) != 0 && *remote_host == 0)
    remote_host = 0;
  if ((tmp = getenv("FIXUP_RECEIVED_HOST")) != 0) {
    if (!str_copys(&fixup_host, tmp)) return &resp_oom;
    str_strip(&fixup_host);
  }
  if ((tmp = getenv("FIXUP_RECEIVED_IP")) != 0) {
    if (!str_copys(&fixup_ip, tmp)) return &resp_oom;
    str_strip(&fixup_ip);
  }
  /* The value of maxdatabytes gets reset in handle_data_start below.
   * This is here simply to provide a value for SMTP to report in its
   * EHLO response. */
  session->maxdatabytes = rules_getenvu("DATABYTES");

  if ((resp = backend_validate_init()) != 0) return resp;
  if ((resp = cvm_validate_init()) != 0) return resp;

  return rules_init();
  (void)session;
}

const response* handle_reset(struct session* session)
{
  const response* resp;
  is_bounce = 0;
  rcpt_count = 0;
  if ((resp = rules_reset()) != 0) return resp;
  session->relayclient = getenv("RELAYCLIENT");
  backend_handle_reset(session);
  return 0;
}

const response* handle_sender(struct session* session, str* sender)
{
  const response* resp;
  const response* tmpresp;
  if (require_auth && !(session->authenticated || session->relayclient != 0))
    return &resp_mustauth;
  /* Logic:
   * if rules_validate_sender returns a response, use it
   * else if backend_validate_sender returns a response, use it
   */
  resp = rules_validate_sender(sender);
  session->maxrcpts = rules_getenvu("MAXRCPTS");
  session->relayclient = rules_getenv("RELAYCLIENT");
  session->maxdatabytes = rules_getenvu("DATABYTES");
  if (resp == 0 && sender->len > 0)
    resp = check_fqdn(sender);
  if (resp == 0)
    resp = backend_validate_sender(sender);
  if (!response_ok(resp))
    return resp;
  if (!response_ok(tmpresp = backend_handle_sender(sender)))
    return tmpresp;
  if (resp == 0) resp = tmpresp;
  is_bounce = sender->len == 0;
  return resp;
}

const response* handle_recipient(struct session* session, str* recip)
{
  const response* resp;
  const response* hresp;
  ++rcpt_count;
  if (session->maxrcpts > 0 && rcpt_count > session->maxrcpts)
    return &resp_manyrcpt;
  if ((resp = rules_validate_recipient(recip)) != 0) {
    if (!number_ok(resp))
      return resp;
  }
  else if ((resp = check_fqdn(recip)) != 0)
    return resp;
  else if (session->relayclient != 0)
    str_cats(recip, session->relayclient);
  else if (session->authenticated)
    resp = 0;
  else if ((resp = backend_validate_recipient(recip)) != 0) {
    if (!number_ok(resp))
      return resp;
  }
  else if ((resp = cvm_validate_recipient(recip)) != 0) {
    if (!number_ok(resp))
      return resp;
  }
  if (!response_ok(hresp = backend_handle_recipient(recip)))
    return hresp;
  if (resp == 0) resp = hresp;
  return resp;
}

static const char* date_string(void)
{
  static char datebuf[64];
  time_t now = time(0);
  struct tm* tm = gmtime(&now);
  strftime(datebuf, sizeof datebuf - 1, "%d %b %Y %H:%M:%S -0000", tm);
  return datebuf;
}

int add_header_add(str* s)
{
  const char* add = rules_getenv("HEADER_ADD");
  if (add != 0) {
    if (!str_cats(s, add)) return 0;
    if (!str_catc(s, '\n')) return 0;
  }
  return 1;
}

int fixup_received(str* s)
{
  if (local_host &&
      local_ip &&
      fixup_host.len > 0 &&
      fixup_ip.len > 0 &&
      (strcasecmp(local_host, fixup_host.s) != 0 ||
       strcasecmp(local_ip, fixup_ip.s) != 0)) {
    if (!str_cat3s(s, "Received: from ", local_host, " (")) return 0;
    if (!str_cat4s(s, local_host, " [", local_ip, "])\n"
		   "  by ")) return 0;
    if (!str_cat(s, &fixup_host)) return 0;
    if (!str_cats(s, " ([")) return 0;
    if (!str_cat(s, &fixup_ip)) return 0;
    if (!str_cat3s(s, "]); ", date_string(), "\n")) return 0;
  }
  return 1;
}

static int str_catfromby(str* s, const char* helo_domain,
			 const char* host, const char* ip)
{
  if (helo_domain == 0)
    helo_domain = (host != 0) ? host : (ip != 0) ? ip : UNKNOWN;
  if (!str_cats(s, helo_domain)) return 0;
  if (host != 0 || ip != 0) {
    if (!str_cats(s, " (")) return 0;
    if (host != 0) {
      if (!str_cats(s, host)) return 0;
      if (ip != 0)
	if (!str_catc(s, ' ')) return 0;
    }
    if (ip != 0)
      if (!str_catc(s, '[') ||
	  !str_cats(s, ip) ||
	  !str_catc(s, ']'))
	return 0;
    if (!str_catc(s, ')')) return 0;
  }
  return 1;
}

int build_received(struct session* session, str* s)
{
  if (!str_cats(s, "Received: from ")) return 0;
  if (!str_catfromby(s, session->helo_domain, remote_host, remote_ip))
    return 0;
  if (!str_cats(s, "\n  by ")) return 0;
  if (!str_catfromby(s, local_host, 0, local_ip)) return 0;
  if (!str_cat4s(s, "\n  with ", session->protocol, " via ", linkproto))
    return 0;
  if (!str_cat3s(s, "; ", date_string(), "\n")) return 0;
  return 1;
}

static unsigned long data_bytes; /* Total number of data bytes */
static unsigned count_rec;	/* Count of the Received: headers */
static unsigned count_dt;	/* Count of the Delivered-To: headers */
static int in_header;		/* True until a blank line is seen */
static unsigned linepos;     /* The number of bytes since the last LF */
static int in_rec;	      /* True if we might be seeing Received: */
static int in_dt;	  /* True if we might be seeing Delivered-To: */
static const response* data_response;

const response* handle_data_start(struct session* session)
{
  const response* resp;
  session->maxdatabytes = rules_getenvu("DATABYTES");
  resp = backend_handle_data_start();
  if (response_ok(resp)) {
    received.len = 0;
    if (!fixup_received(&received) ||
	!add_header_add(&received) ||
	!build_received(session, &received))
      return &resp_internal;
    backend_handle_data_bytes(received.s, received.len);
  }

  if ((session->maxhops = rules_getenvu("MAXHOPS")) == 0)
    session->maxhops = 100;
  
  patterns_init();
  data_bytes = 0;
  count_rec = 0;
  count_dt = 0;
  in_header = 1;
  linepos = 0;
  in_rec = 1;
  in_dt = 1;
  data_response = 0;
  return resp;
}

void handle_data_bytes(struct session* session,
		       const char* bytes, unsigned len)
{
  unsigned i;
  const char* p;
  const response* r;
  data_bytes += len;
  if (data_response) return;
  if (session->maxdatabytes && data_bytes > session->maxdatabytes) {
    data_response = &resp_too_long;
    return;
  }
  if ((r = patterns_check(bytes, len)) != 0) {
    data_response = r;
    return;
  }
  for (i = 0, p = bytes; in_header && i < len; ++i, ++p) {
    char ch = *p;
    if (ch == LF) {
      if (linepos == 0) in_header = 0;
      linepos = 0;
      in_rec = in_dt = in_header;
    }
    else {
      if (in_header && linepos < 13) {
	if (in_rec) {
	  if (ch != "received:"[linepos] &&
	      ch != "RECEIVED:"[linepos])
	    in_rec = 0;
	  else if (linepos >= 8) {
	    in_dt = in_rec = 0;
	    if (++count_rec > session->maxhops) {
	      data_response = &resp_hops;
	      return;
	    }
	  }
	}
	if (in_dt) {
	  if (ch != "delivered-to:"[linepos] &&
	      ch != "DELIVERED-TO:"[linepos])
	    in_dt = 0;
	  else if (linepos >= 12) {
	    in_dt = in_rec = 0;
	    if (++count_dt > session->maxhops) {
	      data_response = &resp_hops;
	      return;
	    }
	  }
	}
	++linepos;
      }
    }
  }
  backend_handle_data_bytes(bytes, len);
  (void)session;
}

const response* handle_data_end(struct session* session)
{
  if (data_response)
    return data_response;
  return backend_handle_data_end();
  (void)session;
}
