sfecho() {
  $src/mailfront smtp echo "$@" 2>/dev/null \
  | grep -v '^220 local.host mailfront ESMTP'
}
sfecho2() {
  $src/mailfront smtp echo "$@" 2>&1 \
  | sed -e '/^220 local.host mailfront ESMTP/d; s/\[[0123456789]*\]:/[###]:/'
}
pfauth() {
  $src/pop3front-auth "$@" echo Yes. 2>/dev/null \
  | tail -n +2
}
ifauth() {
  $src/imapfront-auth sh -c 'echo Yes: $IMAPLOGINTAG' 2>/dev/null \
  | grep -v '^\* OK imapfront ready.'
}
pfmaildir() {
  $src/pop3front-maildir "$@" 2>/dev/null \
  | tail -n +2
}
maildir=$tmp/Maildir
maildir() {
  rm -rf $maildir
  mkdir -p $maildir/cur
  mkdir -p $maildir/new
  mkdir -p $maildir/tmp
}
tstmsg() {
  fn=$1
  {
    echo "Header: foo"
    echo
    echo "body"
  } >$maildir/$fn
}

PROTO=TEST
TESTLOCALIP=1.2.3.4
TESTREMOTEIP=5.6.7.8
TESTLOCALHOST=local.host
TESTREMOTEHOST=remote.host
CVM_PWFILE_PATH=$tmp/pwfile
MODULE_PATH=$src
PLUGINS=accept

export PROTO TESTLOCALIP TESTREMOTEIP TESTLOCALHOST TESTREMOTEHOST
export MAILRULES DATABYTES MAXHOPS PATTERNS MAXNOTIMPL
export PLUGINS MODULE_PATH
