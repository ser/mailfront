#ifndef MAIL_FRONT__MAILFRONT__H__
#define MAIL_FRONT__MAILFRONT__H__

#include "responses.h"
#include <iobuf/iobuf.h>
#include <str/str.h>
#include "constants.h"

struct plugin
{
  struct plugin* next;
  const response* (*init)(void);
  const response* (*helo)(str*);
  const response* (*reset)(void);
  const response* (*sender)(str*);
  const response* (*recipient)(str*);
  const response* (*data_start)(void);
  const response* (*data_block)(const char* bytes, unsigned long len);
  const response* (*data_end)(void);
};

struct protocol
{
  const char* name;
  int (*respond)(const response*);
  int (*init)(void);
  int (*mainloop)(void);
};

struct session
{
  struct protocol* protocol;
  struct plugin* backend;

  const char* helo_domain;

  int authenticated;
  unsigned long maxdatabytes;
  unsigned long maxhops;
  unsigned long maxrcpts;

  str env;
};

extern struct session session;

/* From plugins.c */
extern struct plugin* plugin_list;
extern struct plugin* plugin_tail;
extern void add_plugin(struct plugin*);
extern const response* load_modules(const char* protocol_name,
				    const char* backend_name,
				    const char** plugins);

/* From session.c */
extern void session_init(void);
extern const char* session_getenv(const char* name);
extern unsigned long session_getenvu(const char* name);
extern int session_exportenv(void);
extern int session_setenv(const char* name, const char* value, int overwrite);
extern void session_resetenv(void);

/* From mailfront.c */
extern const char UNKNOWN[];
extern const response* handle_helo(str* host);
extern const response* handle_init(void);
extern const response* handle_reset(void);
extern const response* handle_sender(str* sender);
extern const response* handle_recipient(str* recip);
extern const response* handle_data_start(void);
extern void handle_data_bytes(const char* bytes, unsigned len);
extern const response* handle_data_end(void);
extern int respond(const response*);

/* From netstring.c */
int get_netstring_len(ibuf* in, unsigned long* i);
int get_netstring(ibuf* in, str* s);

/* Defined by a protocol module */
extern int protocol_init(void);
extern int protocol_mainloop(void);

#endif /* MAIL_FRONT__MAILFRONT__H__ */
