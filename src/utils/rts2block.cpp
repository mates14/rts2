#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include "rts2block.h"
#include "rts2command.h"
#include "rts2client.h"
#include "rts2dataconn.h"

#include "imghdr.h"

static Rts2Block *masterBlock = NULL;

Rts2Conn::Rts2Conn (Rts2Block * in_master):Rts2Object ()
{
  sock = -1;
  master = in_master;
  buf_top = buf;
  *name = '\0';
  key = 0;
  priority = -1;
  have_priority = 0;
  centrald_id = -1;
  conn_state = CONN_UNKNOW;
  type = NOT_DEFINED_SERVER;
  runningCommand = NULL;
  for (int i = 0; i < MAX_STATE; i++)
    serverState[i] = NULL;
  otherDevice = NULL;
  otherType = -1;

  connectionTimeout = 300;	// 5 minutes timeout

  time (&lastGoodSend);
  lastData = lastGoodSend;
  lastSendReady = lastGoodSend - connectionTimeout;
}

Rts2Conn::Rts2Conn (int in_sock, Rts2Block * in_master):
Rts2Object ()
{
  sock = in_sock;
  master = in_master;
  buf_top = buf;
  *name = '\0';
  key = 0;
  priority = -1;
  have_priority = 0;
  centrald_id = -1;
  conn_state = CONN_CONNECTED;
  type = NOT_DEFINED_SERVER;
  runningCommand = NULL;
  for (int i = 0; i < MAX_STATE; i++)
    serverState[i] = NULL;
  otherDevice = NULL;
  otherType = -1;

  connectionTimeout = 300;	// 5 minutes timeout (150 + 150)

  time (&lastGoodSend);
  lastData = lastGoodSend;
  lastSendReady = lastData - connectionTimeout;
}

Rts2Conn::~Rts2Conn (void)
{
  if (sock >= 0)
    close (sock);
  for (int i = 0; i < MAX_STATE; i++)
    if (serverState[i])
      delete serverState[i];
  delete otherDevice;
  queClear ();
}

int
Rts2Conn::add (fd_set * set)
{
  if (sock >= 0)
    {
      FD_SET (sock, set);
    }
  return 0;
}

void
Rts2Conn::postEvent (Rts2Event * event)
{
  if (otherDevice)
    otherDevice->postEvent (new Rts2Event (event));
  Rts2Object::postEvent (event);
}

void
Rts2Conn::postMaster (Rts2Event * event)
{
  master->postEvent (event);
}

int
Rts2Conn::idle ()
{
  int ret;
  if (connectionTimeout > 0)
    {
      time_t now;
      time (&now);
      if (now > lastData + getConnTimeout ()
	  && now > lastSendReady + getConnTimeout () / 4)
	{
	  ret = send (PROTO_TECHNICAL " ready");
#ifdef DEBUG_EXTRA
	  syslog (LOG_DEBUG, "Send T ready ret: %i name: '%s' type:%i", ret,
		  getName (), type);
#endif
	  time (&lastSendReady);
	}
      if (now > (lastData + getConnTimeout () * 2))
	{
	  syslog (LOG_DEBUG, "Connection timeout: %li %li %li '%s' type: %i",
		  lastGoodSend, lastData, now, getName (), type);
	  connectionError (-1);
	}
    }
  return 0;
}

int
Rts2Conn::authorizationOK ()
{
  syslog (LOG_ERR, "authorization called on wrong connection");
  return -1;
}

int
Rts2Conn::authorizationFailed ()
{
  syslog (LOG_ERR, "authorization failed on wrong connection");
  return -1;
}

int
Rts2Conn::acceptConn ()
{
  int new_sock;
  struct sockaddr_in other_side;
  socklen_t addr_size = sizeof (struct sockaddr_in);
  new_sock = accept (sock, (struct sockaddr *) &other_side, &addr_size);
  if (new_sock == -1)
    {
      syslog (LOG_ERR, "Rts2Conn::acceptConn data accept %m");
      return -1;
    }
  else
    {
      close (sock);
      sock = new_sock;
#ifdef DEBUG_EXTRA
      syslog (LOG_DEBUG, "Rts2Conn::acceptConn connection accepted");
#endif
      setConnState (CONN_CONNECTED);
      return 0;
    }
}

int
Rts2Conn::setState (int in_state_num, char *in_state_name, int in_value)
{
  if (!serverState[in_state_num])
    serverState[in_state_num] = new Rts2ServerState (in_state_name);
  return setState (in_state_name, in_value);
}

int
Rts2Conn::setState (char *in_state_name, int in_value)
{
  for (int i = 0; i < MAX_STATE; i++)
    {
      Rts2ServerState *state;
      state = serverState[i];
      if (state && !strcmp (state->name, in_state_name))
	{
	  state->setValue (in_value);
	  if (otherDevice)
	    otherDevice->stateChanged (state);
	  return 0;
	}
    }
  return -1;
}

void
Rts2Conn::setOtherType (int other_device_type)
{
  if (otherDevice)
    delete otherDevice;
  otherDevice = master->createOtherType (this, other_device_type);
  otherType = other_device_type;
}

int
Rts2Conn::getOtherType ()
{
  if (otherDevice)
    return otherType;
  return -1;
}

int
Rts2Conn::processLine ()
{
  // starting at command_start, we have complete line, which was
  // received
  int ret;

  // find command parameters end
  command_buf_top = command_start;

  while (*command_buf_top && !isspace (*command_buf_top))
    command_buf_top++;
  // mark command end..
  if (*command_buf_top)
    {
      *command_buf_top = '\0';
      command_buf_top++;
    }
  // priority change
  if (isCommand (PROTO_PRIORITY))
    {
      ret = priorityChange ();
    }
  // informations..
  else if (isCommand (PROTO_INFO))
    {
      ret = informations ();
    }
  // status
  else if (isCommand (PROTO_STATUS))
    {
      ret = status ();
    }
  // technical - to keep connection working
  else if (isCommand (PROTO_TECHNICAL))
    {
      char *msg;
      if (paramNextString (&msg) || !paramEnd ())
	return -1;
      if (!strcmp (msg, "ready"))
	{
#ifdef DEBUG_EXTRA
	  syslog (LOG_DEBUG, "Send T OK");
#endif
	  send (PROTO_TECHNICAL " OK");
	  return -1;
	}
      if (!strcmp (msg, "OK"))
	{
	  return -1;
	}
      return -2;
    }
  else if (isCommandReturn ())
    {
      ret = commandReturn ();
    }
  else
    {
      ret = command ();
    }
#ifdef DEBUG_ALL
  syslog (LOG_DEBUG, "Rts2Conn::processLine [%i] command: %s ret: %i",
	  getCentraldId (), getCommand (), ret);
#endif
  if (!ret)
    sendCommandEnd (0, "OK");
  else if (ret == -2)
    sendCommandEnd (DEVDEM_E_COMMAND,
		    "invalid parameters/invalid number of parameters");
  return ret;
}

int
Rts2Conn::receive (fd_set * set)
{
  int data_size = 0;
  // connections market for deletion
  if (isConnState (CONN_DELETE))
    return -1;
  if ((sock >= 0) && FD_ISSET (sock, set))
    {
      if (isConnState (CONN_CONNECTING))
	{
	  return acceptConn ();
	}
      data_size = read (sock, buf_top, MAX_DATA - (buf_top - buf));
      // ingore EINTR error
      if (data_size == -1 && errno == EINTR)
	return 0;
      if (data_size <= 0)
	return connectionError (data_size);
      buf_top[data_size] = '\0';
      successfullRead ();
#ifdef DEBUG_ALL
      syslog (LOG_DEBUG, "Rts2Conn::receive reas: %s full_buf: %s size: %i",
	      buf_top, buf, data_size);
#endif
      // put old data size into account..
      data_size += buf_top - buf;
      buf_top = buf;
      command_start = buf;
      while (*buf_top)
	{
	  while (isspace (*buf_top) || (*buf_top && *buf_top == '\n'))
	    buf_top++;
	  command_start = buf_top;
	  // find command end..
	  while (*buf_top && *buf_top != '\n' && *buf_top != '\r')
	    buf_top++;

	  // tr "\r\n" "\n\n", so we can deal more easily with it..
	  if (*buf_top == '\r' && *(buf_top + 1) == '\n')
	    *buf_top = '\n';
	  // weird error on when we get \r in one and \n in next read
	  if (*buf_top == '\r' && !*(buf_top + 1))
	    {
	      // we get to 0, while will ends, and we get out, waiting for \n in next packet
	      buf_top++;
	      break;
	    }

	  if (*buf_top == '\n')
	    {
	      // mark end of line..
	      *buf_top = '\0';
	      buf_top++;
	      processLine ();
	      command_start = buf_top;
	    }
	}
      if (buf != command_start)
	{
	  memmove (buf, command_start, (buf_top - command_start + 1));
	  // move buffer to the end..
	  buf_top -= command_start - buf;
	}
    }
  return data_size;
}

int
Rts2Conn::getOurAddress (struct sockaddr_in *in_addr)
{
  // get our address and pass it to data conn
  socklen_t size;
  size = sizeof (struct sockaddr_in);

  return getsockname (sock, (struct sockaddr *) in_addr, &size);
}

void
Rts2Conn::setAddress (struct in_addr *in_address)
{
  addr = *in_address;
}

void
Rts2Conn::getAddress (char *addrBuf, int buf_size)
{
  char *addr_s;
  addr_s = inet_ntoa (addr);
  strncpy (addrBuf, addr_s, buf_size);
  addrBuf[buf_size - 1] = '0';
}

int
Rts2Conn::havePriority ()
{
  return have_priority || master->grantPriority (this);
}

void
Rts2Conn::setCentraldId (int in_centrald_id)
{
  centrald_id = in_centrald_id;
  master->checkPriority (this);
}

int
Rts2Conn::sendPriorityInfo (int number)
{
  char *msg;
  int ret;
  asprintf (&msg, "I status %i priority %i", number, havePriority ());
  ret = send (msg);
  free (msg);
  return ret;
}

int
Rts2Conn::queCommand (Rts2Command * cmd)
{
  cmd->setConnection (this);
  if (runningCommand
      || isConnState (CONN_CONNECTING)
      || isConnState (CONN_AUTH_PENDING) || isConnState (CONN_UNKNOW))
    {
      commandQue.push_back (cmd);
      return 0;
    }
  runningCommand = cmd;
  return cmd->send ();
}

int
Rts2Conn::queSend (Rts2Command * cmd)
{
  cmd->setConnection (this);
  if (isConnState (CONN_CONNECTING) || isConnState (CONN_UNKNOW))
    {
      commandQue.push_front (cmd);
      return 0;
    }
  if (runningCommand)
    commandQue.push_front (runningCommand);
  runningCommand = cmd;
  return runningCommand->send ();
}

int
Rts2Conn::commandReturn (Rts2Command * cmd, int in_status)
{
  if (otherDevice)
    otherDevice->commandReturn (cmd, in_status);
  return 0;
}

void
Rts2Conn::queClear ()
{
  std::list < Rts2Command * >::iterator que_iter;
  for (que_iter = commandQue.begin (); que_iter != commandQue.end ();
       que_iter++)
    {
      Rts2Command *cmd;
      cmd = (*que_iter);
      delete cmd;
    }
  commandQue.clear ();
}

// high-level commands, used to pass variables etc..
int
Rts2Conn::command ()
{
  if (isCommand ("device"))
    {
      int p_centraldId;
      char *p_name;
      char *p_host;
      int p_port;
      int p_device_type;
      if (paramNextInteger (&p_centraldId)
	  || paramNextString (&p_name)
	  || paramNextString (&p_host)
	  || paramNextInteger (&p_port)
	  || paramNextInteger (&p_device_type) || !paramEnd ())
	return -2;
      master->addAddress (p_name, p_host, p_port, p_device_type);
      return -1;
    }
  if (isCommand ("user"))
    {
      int p_centraldId;
      int p_priority;
      char *p_priority_have;
      char *p_login;
      char *p_status_txt = NULL;
      if (paramNextInteger (&p_centraldId)
	  || paramNextInteger (&p_priority)
	  || paramNextString (&p_priority_have)
	  || paramNextString (&p_login)
	  || paramNextStringNull (&p_status_txt))
	return -2;
      master->addUser (p_centraldId, p_priority, (*p_priority_have == '*'),
		       p_login, p_status_txt);
      return -1;
    }
  if (isCommand (PROTO_DATA))
    {
      char *p_command;
      if (paramNextString (&p_command))
	return -2;
      if (!strcmp (p_command, "connect"))
	{
	  int p_chip_id;
	  char *p_hostname;
	  int p_port;
	  int p_size;
	  if (paramNextInteger (&p_chip_id)
	      || paramNextString (&p_hostname)
	      || paramNextInteger (&p_port)
	      || paramNextInteger (&p_size) || !paramEnd ())
	    return -2;
	  master->addDataConnection (this, p_hostname, p_port, p_size);
	  return -1;
	}
      return -2;
    }
  // try otherDevice
  if (otherDevice)
    {
      int ret;
      ret = otherDevice->command ();
      if (ret == 0)
	return -1;
    }
  // don't respond to values with error - otherDevice does respond to
  // values, if there is no other device, we have to take resposibility
  // as it can fails (V without value), not with else
  if (isCommand (PROTO_VALUE))
    return -1;
  syslog (LOG_DEBUG,
	  "Rts2Conn::command unknow command: getCommand %s state: %i type: %i name: %s",
	  getCommand (), conn_state, getType (), getName ());
  sendCommandEnd (-4, "Unknow command");
  return -4;
}

int
Rts2Conn::informations ()
{
  char *sub_command;
  int stat_num;
  char *state_name;
  int value;
  if (paramNextString (&sub_command)
      || paramNextInteger (&stat_num)
      || paramNextString (&state_name)
      || paramNextInteger (&value) || !paramEnd ())
    return -2;
  // set initial state & name
  setState (stat_num, state_name, value);
  return -1;
}

int
Rts2Conn::status ()
{
  char *stat_name;
  int value;
  char *stat_text;
  if (paramNextString (&stat_name)
      || paramNextInteger (&value) || paramNextStringNull (&stat_text))
    return -2;
  setState (stat_name, value);
  return -1;
}

int
Rts2Conn::sendNextCommand ()
{
  // pop next command
  if (commandQue.size () > 0)
    {
      runningCommand = *(commandQue.begin ());
      commandQue.erase (commandQue.begin ());
      runningCommand->send ();
      return 0;
    }
  runningCommand = NULL;
  return -1;
}

int
Rts2Conn::commandReturn ()
{
  int ret;
  // ignore (for the moment) retuns recieved without command
  if (!runningCommand)
    {
#ifdef DEBUG_EXTRA
      syslog (LOG_DEBUG, "Rts2Conn::commandReturn null!");
#endif
      return -1;
    }
  ret = runningCommand->commandReturn (atoi (getCommand ()));
  switch (ret)
    {
    case RTS2_COMMAND_REQUE:
      if (runningCommand)
	runningCommand->send ();
      break;
    case -1:
      delete runningCommand;
      sendNextCommand ();
      break;
    }
  return -1;
}

int
Rts2Conn::priorityChange ()
{
  // we don't want any messages yet..
  return -1;
}

int
Rts2Conn::send (char *msg)
{
  int len;
  int ret;

  if (sock == -1)
    return -1;

  len = strlen (msg);

  ret = write (sock, msg, len);

  if (ret != len)
    {
#ifdef DEBUG_EXTRA
      syslog (LOG_ERR,
	      "Rts2Conn::send [%i:%i] error %i state: %i sending '%s':%m",
	      getCentraldId (), conn_state, sock, ret, msg);
#endif
      connectionError (ret);
      return -1;
    }
#ifdef DEBUG_ALL
  syslog (LOG_DEBUG, "Rts2Conn::send [%i:%i] send %i: '%s'", getCentraldId (),
	  sock, ret, msg);
#endif
  write (sock, "\r\n", 2);
  successfullSend ();
  return 0;
}

void
Rts2Conn::successfullSend ()
{
  time (&lastGoodSend);
}

void
Rts2Conn::getSuccessSend (time_t * in_t)
{
  *in_t = lastGoodSend;
}

int
Rts2Conn::reachedSendTimeout ()
{
  time_t now;
  time (&now);
  return now > lastGoodSend + getConnTimeout ();
}

void
Rts2Conn::successfullRead ()
{
  time (&lastData);
}

int
Rts2Conn::connectionError (int last_data_size)
{
  setConnState (CONN_DELETE);
  if (sock >= 0)
    close (sock);
  sock = -1;
  if (strlen (getName ()))
    master->deleteAddress (getName ());
  return -1;
}

int
Rts2Conn::sendValue (char *val_name, int value)
{
  char *msg;
  int ret;

  asprintf (&msg, "V %s %i", val_name, value);
  ret = send (msg);
  free (msg);
  return ret;
}

int
Rts2Conn::sendValue (char *val_name, int val1, int val2)
{
  char *msg;
  int ret;

  asprintf (&msg, "V %s %i %i", val_name, val1, val2);
  ret = send (msg);
  free (msg);
  return ret;
}

int
Rts2Conn::sendValue (char *val_name, int val1, double val2)
{
  char *msg;
  int ret;

  asprintf (&msg, "V %s %i %f", val_name, val1, val2);
  ret = send (msg);
  free (msg);
  return ret;
}

int
Rts2Conn::sendValue (char *val_name, char *value)
{
  char *msg;
  int ret;

  asprintf (&msg, "V %s %s", val_name, value);
  ret = send (msg);
  free (msg);
  return ret;
}

int
Rts2Conn::sendValue (char *val_name, double value)
{
  char *msg;
  int ret;

  asprintf (&msg, "V %s %f", val_name, value);
  ret = send (msg);
  free (msg);
  return ret;
}

int
Rts2Conn::sendValue (char *val_name, char *val1, int val2)
{
  char *msg;
  int ret;

  asprintf (&msg, "V %s %s %i", val_name, val1, val2);
  ret = send (msg);
  free (msg);
  return ret;
}

int
Rts2Conn::sendValue (char *val_name, int val1, int val2, double val3,
		     double val4, double val5, double val6)
{
  char *msg;
  int ret;

  asprintf (&msg, "V %s %i %i %f %f %f %f", val_name, val1, val2, val3, val4,
	    val5, val6);
  ret = send (msg);
  free (msg);
  return ret;
}

int
Rts2Conn::sendValueTime (char *val_name, time_t * value)
{
  char *msg;
  int ret;

  asprintf (&msg, "V %s %li", val_name, *value);
  ret = send (msg);
  free (msg);
  return ret;
}

int
Rts2Conn::sendCommandEnd (int num, char *in_msg)
{
  char *msg;

  asprintf (&msg, "%+04i %s", num, in_msg);
  send (msg);
  free (msg);

  return 0;
}

void
Rts2Conn::setConnState (conn_state_t new_conn_state)
{
  if (new_conn_state == CONN_AUTH_OK || new_conn_state == CONN_CONNECTED)
    {
      if (!runningCommand)
	sendNextCommand ();
      // otherwise command will be send after command which trigerred
      // state change finished..
    }
  conn_state = new_conn_state;
  if (new_conn_state == CONN_AUTH_FAILED)
    {
      connectionError (-1);
    }
}

int
Rts2Conn::isConnState (conn_state_t in_conn_state)
{
  return (conn_state == in_conn_state);
}

int
Rts2Conn::paramEnd ()
{
  return !*command_buf_top;
}

int
Rts2Conn::paramNextString (char **str)
{
  while (isspace (*command_buf_top))
    command_buf_top++;
  *str = command_buf_top;
  if (!*command_buf_top)
    return -1;
  while (!isspace (*command_buf_top) && *command_buf_top)
    command_buf_top++;
  if (*command_buf_top)
    {
      *command_buf_top = '\0';
      command_buf_top++;
    }
  return 0;
}

int
Rts2Conn::paramNextStringNull (char **str)
{
  int ret;
  ret = paramNextString (str);
  if (ret)
    return !paramEnd ();
  return ret;
}

int
Rts2Conn::paramNextInteger (int *num)
{
  char *str_num;
  char *num_end;
  if (paramNextString (&str_num))
    return -1;
  *num = strtol (str_num, &num_end, 10);
  if (*num_end)
    return -1;
  return 0;
}

int
Rts2Conn::paramNextDouble (double *num)
{
  char *str_num;
  char *num_end;
  if (paramNextString (&str_num))
    return -1;
  if (!strcmp (str_num, "nan"))
    {
      *num = nan ("f");
      return 0;
    }
  *num = strtod (str_num, &num_end);
  if (*num_end)
    return -1;
  return 0;
}

int
Rts2Conn::paramNextFloat (float *num)
{
  char *str_num;
  char *num_end;
  if (paramNextString (&str_num))
    return -1;
  *num = strtof (str_num, &num_end);
  if (*num_end)
    return -1;
  return 0;
}

void
Rts2Conn::dataReceived (Rts2ClientTCPDataConn * dataConn)
{
  if (otherDevice)
    {
      otherDevice->dataReceived (dataConn);
    }
}

Rts2Value *
Rts2Conn::getValue (const char *value_name)
{
  if (otherDevice)
    {
      return otherDevice->getValue (value_name);
    }
  return NULL;
}

Rts2Block::Rts2Block (int in_argc, char **in_argv):
Rts2App (in_argc, in_argv)
{
  idle_timeout = USEC_SEC * 10;
  priority_client = -1;
  openlog (NULL, LOG_PID, LOG_LOCAL0);
  for (int i = 0; i < MAX_CONN; i++)
    {
      connections[i] = NULL;
    }
  signal (SIGPIPE, SIG_IGN);

  masterState = 0;
  // allocate ports dynamically
  port = 0;
  mailAddress = NULL;
}

Rts2Block::~Rts2Block (void)
{
  int i;
  for (i = 0; i < MAX_CONN; i++)
    {
      Rts2Conn *conn;
      conn = connections[i];
      if (conn)
	delete conn;
      connections[i] = NULL;
    }
}

void
Rts2Block::setPort (int in_port)
{
  port = in_port;
}

int
Rts2Block::getPort (void)
{
  return port;
}

void
signalHUP (int sig)
{
  if (masterBlock)
    masterBlock->sigHUP (sig);
}

int
Rts2Block::init ()
{
  int ret;
  ret = initOptions ();
  if (ret)
    return ret;

  masterBlock = this;
  signal (SIGHUP, signalHUP);
  return 0;
}

void
Rts2Block::forkedInstance ()
{
}

void
Rts2Block::postEvent (Rts2Event * event)
{
  // send to all connections
  for (int i = 0; i < MAX_CONN; i++)
    {
      Rts2Conn *conn;
      conn = connections[i];
      if (conn)
	{
	  conn->postEvent (new Rts2Event (event));
	}
    }
  return Rts2App::postEvent (event);
}

Rts2Conn *
Rts2Block::createConnection (int in_sock, int conn_num)
{
  return new Rts2Conn (in_sock, this);
}

Rts2Conn *
Rts2Block::addDataConnection (Rts2Conn * owner_conn, char *in_hostname,
			      int in_port, int in_size)
{
  Rts2Conn *conn;
  conn =
    new Rts2ClientTCPDataConn (this, owner_conn, in_hostname, in_port,
			       in_size);
  addConnection (conn);
  return conn;
}

int
Rts2Block::addConnection (Rts2Conn * conn)
{
  int i;
  for (i = 0; i < MAX_CONN; i++)
    {
      if (!connections[i])
	{
#ifdef DEBUG_EXTRA
	  syslog (LOG_DEBUG, "Rts2Block::addConnection add conn: %i", i);
#endif
	  connections[i] = conn;
	  return 0;
	}
    }
  syslog (LOG_ERR,
	  "Rts2Block::addConnection Cannot find empty connection!\n");
  return -1;
}

Rts2Conn *
Rts2Block::findName (const char *in_name)
{
  int i;
  for (i = 0; i < MAX_CONN; i++)
    {
      Rts2Conn *conn = connections[i];
      if (conn)
	if (!strcmp (conn->getName (), in_name))
	  return conn;
    }
  // if connection not found, try to look to list of available
  // connections
  return NULL;
}

Rts2Conn *
Rts2Block::findCentralId (int in_id)
{
  int i;
  for (i = 0; i < MAX_CONN; i++)
    {
      Rts2Conn *conn = connections[i];
      if (conn)
	if (conn->getCentraldId () == in_id)
	  return conn;
    }
  return NULL;
}

int
Rts2Block::sendAll (char *message)
{
  int i;
  for (i = 0; i < MAX_CONN; i++)
    {
      Rts2Conn *conn = connections[i];
      if (conn)
	{
	  conn->send (message);
	}
    }
  return 0;
}

int
Rts2Block::sendPriorityChange (int p_client, int timeout)
{
  char *msg;
  int ret;

  asprintf (&msg, PROTO_PRIORITY " %i %i", p_client, timeout);
  ret = sendAll (msg);
  free (msg);
  return ret;
}

int
Rts2Block::sendStatusMessage (char *state_name, int state)
{
  char *msg;
  int ret;

  asprintf (&msg, PROTO_STATUS " %s %i", state_name, state);
  ret = sendAll (msg);
  free (msg);
  return ret;
}

int
Rts2Block::idle ()
{
  int ret;
  Rts2Conn *conn;
  ret = waitpid (-1, NULL, WNOHANG);
  if (ret > 0)
    {
      childReturned (ret);
    }
  for (int i = 0; i < MAX_CONN; i++)
    {
      conn = connections[i];
      if (conn)
	conn->idle ();
    }
  return 0;
}

void
Rts2Block::addSelectSocks (fd_set * read_set)
{
  Rts2Conn *conn;
  int i;
  for (i = 0; i < MAX_CONN; i++)
    {
      conn = connections[i];
      if (conn)
	{
	  conn->add (read_set);
	}
    }
}

void
Rts2Block::selectSuccess (fd_set * read_set)
{
  int i;
  Rts2Conn *conn;
  int ret;

  for (i = 0; i < MAX_CONN; i++)
    {
      conn = connections[i];
      if (conn)
	{
	  if (conn->receive (read_set) == -1)
	    {
#ifdef DEBUG_EXTRA
	      syslog (LOG_ERR,
		      "Will delete connection %i, name: '%s'", i,
		      conn->getName ());
#endif
	      ret = deleteConnection (conn);
	      // delete connection only when it really requested to be deleted..
	      if (!ret)
		connections[i] = NULL;
	    }
	}
    }
}

int
Rts2Block::run ()
{
  int ret;
  struct timeval read_tout;
  fd_set read_set;

  end_loop = 0;

  while (!end_loop)
    {
      read_tout.tv_sec = idle_timeout / USEC_SEC;
      read_tout.tv_usec = idle_timeout % USEC_SEC;

      FD_ZERO (&read_set);
      addSelectSocks (&read_set);
      ret = select (FD_SETSIZE, &read_set, NULL, NULL, &read_tout);
      if (ret > 0)
	{
	  selectSuccess (&read_set);
	}
      ret = idle ();
      if (ret == -1)
	break;
    }
  return 0;
}

int
Rts2Block::deleteConnection (Rts2Conn * conn)
{
  if (conn->havePriority ())
    {
      cancelPriorityOperations ();
    }
  if (conn->isConnState (CONN_DELETE))
    {
      delete conn;
      return 0;
    }
  // don't delete us when we are in incorrect state
  return -1;
}

int
Rts2Block::setPriorityClient (int in_priority_client, int timeout)
{
  int discard_priority = 1;
  Rts2Conn *priConn;
  priConn = findCentralId (in_priority_client);
  if (priConn && priConn->getHavePriority ())
    discard_priority = 0;

  for (int i = 0; i < MAX_CONN; i++)
    {
      if (connections[i]
	  && connections[i]->getCentraldId () == in_priority_client)
	{
	  if (discard_priority)
	    {
	      cancelPriorityOperations ();
	      if (priConn)
		priConn->setHavePriority (0);
	    }
	  connections[i]->setHavePriority (1);
	  break;
	}
    }
  priority_client = in_priority_client;
  return 0;
}

void
Rts2Block::cancelPriorityOperations ()
{
}

void
Rts2Block::childReturned (pid_t child_pid)
{
#ifdef DEBUG_EXTRA
  syslog (LOG_DEBUG, "child returned: %i", child_pid);
#endif
  for (int i = 0; i < MAX_CONN; i++)
    {
      Rts2Conn *conn;
      conn = connections[i];
      if (conn)
	{
	  conn->childReturned (child_pid);
	}
    }
}

int
Rts2Block::willConnect (Rts2Address * in_addr)
{
  return 0;
}

int
Rts2Block::sendMail (char *subject, char *text)
{
  // no mail will be send
  if (!mailAddress)
    return 0;

  return sendMailTo (subject, text, mailAddress, this);
}

Rts2Address *
Rts2Block::findAddress (const char *blockName)
{
  std::list < Rts2Address * >::iterator addr_iter;

  for (addr_iter = blockAddress.begin (); addr_iter != blockAddress.end ();
       addr_iter++)
    {
      Rts2Address *addr = (*addr_iter);
      if (addr->isAddress (blockName))
	{
	  return addr;
	}
    }
  return NULL;
}

void
Rts2Block::addAddress (const char *p_name, const char *p_host, int p_port,
		       int p_device_type)
{
  int ret;
  Rts2Address *an_addr;
  an_addr = findAddress (p_name);
  if (an_addr)
    {
      ret = an_addr->update (p_name, p_host, p_port, p_device_type);
      if (!ret)
	{
	  addAddress (an_addr);
	  return;
	}
    }
  an_addr = new Rts2Address (p_name, p_host, p_port, p_device_type);
  blockAddress.push_back (an_addr);
  addAddress (an_addr);
}

int
Rts2Block::addAddress (Rts2Address * in_addr)
{
  Rts2Conn *conn;
  // recheck all connections waiting for our address
  conn = getOpenConnection (in_addr->getName ());
  if (conn)
    conn->addressUpdated (in_addr);
  else if (willConnect (in_addr))
    {
      conn = createClientConnection (in_addr);
      if (conn)
	addConnection (conn);
    }
  return 0;
}

void
Rts2Block::deleteAddress (const char *p_name)
{
  std::list < Rts2Address * >::iterator addr_iter;

  for (addr_iter = blockAddress.begin (); addr_iter != blockAddress.end ();
       addr_iter++)
    {
      Rts2Address *addr = (*addr_iter);
      if (addr->isAddress (p_name))
	{
	  blockAddress.erase (addr_iter);
	  delete addr;
	  return;
	}
    }
}

Rts2DevClient *
Rts2Block::createOtherType (Rts2Conn * conn, int other_device_type)
{
  switch (other_device_type)
    {
    case DEVICE_TYPE_MOUNT:
      return new Rts2DevClientTelescope (conn);
    case DEVICE_TYPE_CCD:
      return new Rts2DevClientCamera (conn);
    case DEVICE_TYPE_DOME:
      return new Rts2DevClientDome (conn);
    case DEVICE_TYPE_COPULA:
      return new Rts2DevClientCopula (conn);
    case DEVICE_TYPE_PHOT:
      return new Rts2DevClientPhot (conn);
    case DEVICE_TYPE_FW:
      return new Rts2DevClientFilter (conn);
    case DEVICE_TYPE_EXECUTOR:
      return new Rts2DevClientExecutor (conn);
    case DEVICE_TYPE_IMGPROC:
      return new Rts2DevClientImgproc (conn);
    case DEVICE_TYPE_SELECTOR:
      return new Rts2DevClientSelector (conn);
    case DEVICE_TYPE_GRB:
      return new Rts2DevClientGrb (conn);

    default:
      return new Rts2DevClient (conn);
    }
}

void
Rts2Block::addUser (int p_centraldId, int p_priority, char p_priority_have,
		    const char *p_login, const char *p_status_txt)
{
  int ret;
  std::list < Rts2User * >::iterator user_iter;
  for (user_iter = blockUsers.begin (); user_iter != blockUsers.end ();
       user_iter++)
    {
      ret =
	(*user_iter)->update (p_centraldId, p_priority, p_priority_have,
			      p_login, p_status_txt);
      if (!ret)
	return;
    }
  addUser (new
	   Rts2User (p_centraldId, p_priority, p_priority_have, p_login,
		     p_status_txt));
}

int
Rts2Block::addUser (Rts2User * in_user)
{
  blockUsers.push_back (in_user);
  return 0;
}

Rts2Conn *
Rts2Block::getOpenConnection (char *deviceName)
{
  Rts2Conn *conn;

  // try to find active connection..
  for (int i = 0; i < MAX_CONN; i++)
    {
      conn = connections[i];
      if (!conn)
	continue;
      if (conn->isName (deviceName))
	return conn;
    }
  return NULL;
}

Rts2Conn *
Rts2Block::getConnection (char *deviceName)
{
  Rts2Conn *conn;
  Rts2Address *devAddr;

  conn = getOpenConnection (deviceName);
  if (conn)
    return conn;

  devAddr = findAddress (deviceName);

  if (!devAddr)
    {
#ifdef DEBUG_EXTRA
      syslog (LOG_ERR,
	      "Cannot find device with name '%s', creating new connection",
	      deviceName);
#endif
      conn = createClientConnection (deviceName);
      addConnection (conn);
      return conn;
    }

  // open connection to given address..
  conn = createClientConnection (devAddr);
  addConnection (conn);
  return conn;
}

int
Rts2Block::queAll (Rts2Command * command)
{
  // go throught all adresses
  std::list < Rts2Address * >::iterator addr_iter;
  Rts2Conn *conn;

  for (addr_iter = blockAddress.begin (); addr_iter != blockAddress.end ();
       addr_iter++)
    {
      conn = getConnection ((*addr_iter)->getName ());
      if (conn)
	{
	  Rts2Command *newCommand = new Rts2Command (command);
	  conn->queCommand (newCommand);
	}
      else
	{
#ifdef DEBUG_EXTRA
	  syslog (LOG_DEBUG, "Rts2Block::queAll no connection for %s",
		  (*addr_iter)->getName ());
#endif
	}
    }
  delete command;
  return 0;
}

int
Rts2Block::queAll (char *text)
{
  return queAll (new Rts2Command (this, text));
}

int
Rts2Block::allQuesEmpty ()
{
  int ret;
  for (int i = 0; i < MAX_CONN; i++)
    {
      Rts2Conn *conn;
      conn = connections[i];
      if (conn)
	{
	  ret = conn->queEmpty ();
	  if (!ret)
	    return ret;
	}
    }
  return ret;
}

Rts2Conn *
Rts2Block::getMinConn (const char *valueName)
{
  int lovestValue = INT_MAX;
  Rts2Conn *minConn = NULL;
  for (int i = 0; i < MAX_CONN; i++)
    {
      Rts2Value *que_size;
      Rts2Conn *conn;
      conn = connections[i];
      if (conn)
	{
	  que_size = conn->getValue (valueName);
	  if (que_size)
	    {
	      if (que_size->getValueInteger () >= 0
		  && que_size->getValueInteger () < lovestValue)
		{
		  minConn = conn;
		  lovestValue = que_size->getValueInteger ();
		}
	    }
	}
    }
  return minConn;
}

void
Rts2Block::sigHUP (int sig)
{
}
