/* 
 * dome driver skeleton.
 * Copyright (C) 2005-2008 Petr Kubanek <petr@kubanek.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "dome.h"

using namespace rts2dome;
using namespace rts2core;

#define OPT_WEATHER_OPENS     OPT_LOCAL + 200
#define OPT_STATE_MASTER      OPT_LOCAL + 201
#define OPT_DONOTCLOSE        OPT_LOCAL + 202
#define OPT_IGNORETIMEOUT     OPT_LOCAL + 203

int Dome::domeOpenStart ()
{
	if (isOpened () == -2)
		return 0;

	if (isGoodWeather () == false)
		return -1;

	if (startOpen ())
	{
	  	logStream (MESSAGE_ERROR) << "opening of the dome failed" << sendLog;
		return -1;
	}

	closeFailReported = false;

	maskState (DOME_DOME_MASK | BOP_EXPOSURE, DOME_OPENING | BOP_EXPOSURE, "opening dome");
	logStream (MESSAGE_REPORTIT | MESSAGE_INFO) << "starting to open the dome" << sendLog;
	return 0;
}

int Dome::domeCloseStart ()
{
	if (isClosed () == -2)
		return 0;

	int ret = startClose ();

	if (ret < 0)
	{
		if (closeFailReported == false)
			logStream (MESSAGE_REPORTIT | MESSAGE_ERROR) << "cannot start closing of the dome" << sendLog;
		closeFailReported = true;
		return -1;
	}
	else if (ret > 0)
	{
		if ((getState () & DOME_DOME_MASK) != DOME_WAIT_CLOSING)
		{
			logStream (MESSAGE_REPORTIT | MESSAGE_INFO) << "wait for equipment to stow" << sendLog;
		}
		maskState (DOME_DOME_MASK | BOP_EXPOSURE, DOME_WAIT_CLOSING, "wait for equipment to stow");
		return 0;
	}

	closeFailReported = false;

	if ((getState () & DOME_DOME_MASK) != DOME_CLOSING)
	{
		logStream (MESSAGE_REPORTIT | MESSAGE_INFO) << "closing dome" << sendLog;
	}
	maskState (DOME_DOME_MASK | BOP_EXPOSURE, DOME_CLOSING, "closing dome");
	return 0;
};

Dome::Dome (int in_argc, char **in_argv, int in_device_type):Device (in_argc, in_argv, in_device_type, "DOME")
{
	stateMaster = NULL;

	closeFailReported = false;

	createValue (weatherOpensDome, "weather_open", "if true, good weather can open dome", false, RTS2_VALUE_WRITABLE | RTS2_VALUE_AUTOSAVE);
	weatherOpensDome->setValueBool (false);

	createValue (ignoreTimeout, "ignore_time", "ignore weather state until given time", false, RTS2_VALUE_WRITABLE);
	ignoreTimeout->setValueDouble (0);

	createValue (nextGoodWeather, "next_open", "next possible opening of the dome", false);
	nextGoodWeather->setValueDouble (getNow () + DEF_WEATHER_TIMEOUT);

	addOption (OPT_WEATHER_OPENS, "weather-can-open", 0, "specified that option if weather signal is allowed to open dome");
	addOption (OPT_STATE_MASTER, "state-master", 1, "state master - server which guverns opening and closing of dome for evening and morning");
	addOption (OPT_DONOTCLOSE, "notclose", 0, "do not close (switch to bad weather) on startup");
	addOption (OPT_IGNORETIMEOUT, "ignore-timeout", 1, "set initial ignore timeout to now + value in seconds");
}


Dome::~Dome ()
{
}

int Dome::processOption (int in_opt)
{
	switch (in_opt)
	{
		case OPT_WEATHER_OPENS:
			weatherOpensDome->setValueBool (true);
			break;
		case OPT_STATE_MASTER:
			createValue (stateMaster, "state_master", "server governing dome evening and morning states", false);
			updateMetaInformations (stateMaster);
			stateMaster->setValueCharArr (optarg);
			break;
		case OPT_IGNORETIMEOUT:
			ignoreTimeout->setValueDouble (getNow () + atoi (optarg));
			break;
		case OPT_DONOTCLOSE:
			nextGoodWeather->setValueDouble (getNow () - 300);
			break;
		default:
			return Device::processOption (in_opt);
	}
	return 0;
}

int Dome::init ()
{
	int ret;
	ret = Device::init ();
	if (ret)
		return ret;

	// check for presense of state-master
	if (getCentraldConns ()->size () > 1)
	{
 		if (stateMaster == NULL || stateMaster->getValueString ().length () == 0)
		{
			logStream (MESSAGE_ERROR) << "multiple central server specified, but none identified as master - please add state-master option" << sendLog;
			return -1;
		}
		rts2core::Connection *conn = getCentraldConn (stateMaster->getValue ());
		if (conn == NULL)
		{
			logStream (MESSAGE_ERROR) << "cannot find state-master " << stateMaster->getValue () << " in list of central connections" << sendLog;
			return -1;
		}
		setMasterConn (conn);
	}
	return 0;
}

bool Dome::isGoodWeather ()
{
	if (getIgnoreMeteo () == true)
		return true;
	if (Device::isGoodWeather () == false)
	  	return false;
	if (getNextOpen () > getNow ())
		return false;
	return true;
}

int Dome::checkOpening ()
{
	if ((getState () & DOME_DOME_MASK) == DOME_OPENING)
	{
		long ret;
		ret = isOpened ();
		if (ret >= 0)
		{
			setTimeout (ret);
			return 0;
		}
		if (ret == -1)
		{
			endOpen ();
			infoAll ();
			logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "dome opening interrupted" << sendLog;
			maskState (DOME_DOME_MASK | BOP_EXPOSURE, DOME_OPENED, "dome opening interrupted");
		}
		if (ret == -2)
		{
			ret = endOpen ();
			infoAll ();
			if (ret)
			{
				logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "dome did not open propely" << sendLog;
				maskState (DOME_DOME_MASK | BOP_EXPOSURE, DOME_OPENED, "dome did not open properly");
			}
			else
			{
				maskState (DOME_DOME_MASK | BOP_EXPOSURE, DOME_OPENED, "dome opened");
			}
		}
	}
	else if ((getState () & DOME_DOME_MASK) == DOME_CLOSING)
	{
		long ret;
		ret = isClosed ();
		if (ret >= 0)
		{
			setTimeout (ret);
			return 0;
		}
		if (ret == -1)
		{
			endClose ();
			infoAll ();
			logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "dome closing interrupted" << sendLog;
			maskState (DOME_DOME_MASK, DOME_CLOSED, "closing finished with error");
		}
		if (ret == -2)
		{
			ret = endClose ();
			infoAll ();
			if (ret)
			{
				logStream (MESSAGE_CRITICAL | MESSAGE_REPORTIT) << "dome did not close properly" << sendLog;
				maskState (DOME_DOME_MASK, DOME_CLOSED, "dome did not close propely");
			}
			else
			{
				maskState (DOME_DOME_MASK, DOME_CLOSED, "dome closed");
				logStream (MESSAGE_INFO) << "dome closed" << sendLog;
			}
		}
	}
	// if we are back in idle state..beware of copula state (bit non-structural, but I
	// cannot find better solution)
	if ((getState () & DOME_CUP_MASK_MOVE) == DOME_CUP_NOT_MOVE)
		setTimeout (10 * USEC_SEC);
	return 0;
}

int Dome::idle ()
{
	checkOpening ();
	if (isGoodWeather ())
	{
		if (weatherOpensDome->getValueBool () == true && (getMasterState () & SERVERD_ONOFF_MASK) == SERVERD_STANDBY)
		{
			setMasterOn ();
		}
	}
	else 
	{
		int ret;
		ret = closeDomeWeather ();
		if (ret == -1)
		{
			setTimeout (10 * USEC_SEC);
		}
	}
	// update our own weather state..
	bool allCen = allCentraldRunning ();
	if (allCen && getNextOpen () < getNow ())
	{
		valueGood (nextGoodWeather);
		setWeatherState (true, "can open dome");
	}
	else
	{
		if (allCen)
			setWeatherState (false, "waiting for next_open");
		else
		  	setWeatherState (false, "some centrald are not connected");
	}

	return Device::idle ();
}

int Dome::closeDomeWeather ()
{
	int ret;
	if (getIgnoreMeteo () == false)
	{
		ret = domeCloseStart ();
		setMasterStandby ();
		return ret;
	}
	return 0;
}

int Dome::observing ()
{
	return domeOpenStart ();
}

int Dome::standby ()
{
	ignoreTimeout->setValueDouble (getNow () - 1);
	return domeCloseStart ();
}

int Dome::off ()
{
	ignoreTimeout->setValueDouble (getNow () - 1);
	return domeCloseStart ();
}

int Dome::setMasterStandby ()
{
	if (!(getMasterState () & SERVERD_ONOFF_MASK) && ((getMasterState () & SERVERD_STATUS_MASK) != SERVERD_UNKNOW))
	{
		return sendMasters ("standby");
	}
	return 0;
}

int Dome::setMasterOn ()
{
	if ((getMasterState () & SERVERD_ONOFF_MASK) == SERVERD_STANDBY)
	{
		return sendMasters ("on");
	}
	return 0;
}

void Dome::changeMasterState (rts2_status_t old_state, rts2_status_t new_state)
{
	// detect state changes triggered by bad weather
	if ((old_state & WEATHER_MASK) == GOOD_WEATHER && (new_state & WEATHER_MASK) == BAD_WEATHER && getIgnoreMeteo () == true)
	{
		logStream (MESSAGE_INFO) << "ignoring bad weather trigerred state change" << sendLog;
	}
	else if ((new_state & SERVERD_ONOFF_MASK) == SERVERD_STANDBY)
	{
		switch (new_state & SERVERD_STATUS_MASK)
		{
			case SERVERD_EVENING:
			case SERVERD_DUSK:
			case SERVERD_NIGHT:
			case SERVERD_DAWN:
				standby ();
				break;
			default:
				off ();
		}
	}
	// HARD_OFF or SOFT_OFF states - STANDBY is above
	else if (new_state & SERVERD_ONOFF_MASK)
	{
		off ();
	}
	else
	{
		switch (new_state & SERVERD_STATUS_MASK)
		{
			case SERVERD_DUSK:
			case SERVERD_NIGHT:
			case SERVERD_DAWN:
				observing ();
				break;
			case SERVERD_EVENING:
			case SERVERD_MORNING:
				standby ();
				break;
			default:
				off ();
		}
	}
	Device::changeMasterState (old_state, new_state);
}

void Dome::setIgnoreTimeout (time_t _ignore_time)
{
	time_t now;
	time (&now);
	now += _ignore_time;
}

bool Dome::getIgnoreMeteo ()
{
	return ignoreTimeout->getValueDouble () > getNow ();
}

void Dome::setWeatherTimeout (time_t wait_time, const char *msg)
{
	time_t next;
	time (&next);
	next += wait_time;
	if (next > nextGoodWeather->getValueInteger ())
	{
		valueError (nextGoodWeather);
		setWeatherState (false, msg);
		nextGoodWeather->setValueInteger (next);
		sendValueAll (nextGoodWeather);
	}
}

int Dome::commandAuthorized (rts2core::Connection * conn)
{
	if (conn->isCommand ("open"))
	{
		return (domeOpenStart () == 0 ? 0 : -2);
	}
	else if (conn->isCommand ("close"))
	{
		return (domeCloseStart () == 0 ? 0 : -2);
	}
	else if (conn->isCommand ("close_for"))
	{
		double next_good;
		if (conn->paramNextDouble (&next_good) || !conn->paramEnd())
			return -2;
		if (next_good < 0)
			return -2;
		nextGoodWeather->setValueDouble (getNow () + next_good);
		valueError (nextGoodWeather);
		return (domeCloseStart () == 0 ? 0 : -2);
	}
	else if (conn->isCommand ("reset_next"))
	{
		nextGoodWeather->setValueDouble (getNow () - 1);
		valueGood (nextGoodWeather);
		return 0;
	}
	return Device::commandAuthorized (conn);
}
