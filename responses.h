#ifndef MAIL_FRONT__RESPONSES__H__
#define MAIL_FRONT__RESPONSES__H__

struct response 
{
  unsigned number;
  const char* message;
};
typedef struct response response;

extern const response resp_oom;
#define RESPONSE(NAME,CODE,MSG) const response resp_##NAME = {CODE,MSG}

#endif
