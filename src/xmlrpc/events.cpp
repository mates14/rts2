/* 
 * Infrastructure which supports events generated by triggers.
 * Copyright (C) 2009 Petr Kubanek <petr@kubanek.net>
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

#include "events.h"

#include "stateevents.h"
#include "message.h"

#include "../utils/rts2block.h"
#include "../utils/rts2logstream.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

using namespace rts2xmlrpc;

void Events::parseState (xmlNodePtr event, std::string deviceName)
{
	xmlAttrPtr properties = event->properties;
	int changeMask = -1;
	int newStateValue = -1;
	std::string commandName;
	std::string condition;

	for (; properties; properties = properties->next)
	{
		if (xmlStrEqual (properties->name, (xmlChar *) "state-mask"))
		{
			changeMask = atoi ((char *) properties->children->content);
		}
		else if (xmlStrEqual (properties->name, (xmlChar *) "state"))
		{
			newStateValue = atoi ((char *) properties->children->content);
		}
		else if (xmlStrEqual (properties->name, (xmlChar *) "if"))
		{
			condition = std::string ((char *) properties->children->content);
			std::cout << "if " << condition << std::endl;
		}
		else
		{
			throw XmlUnexpectedAttribute (event, (char *) properties->name);
		}
	}
	if (newStateValue < 0)
		throw XmlUnexpectedAttribute (event, "state");

	if (changeMask < 0)
		changeMask = newStateValue;
	
	xmlNodePtr action = event->children;
	if (action == NULL)
		throw XmlEmptyNode (event);

	for (; action != NULL; action = action->next)
	{
		if (xmlStrEqual (action->name, (xmlChar *) "record"))
		{
			stateCommands.push_back (new StateChangeRecord (deviceName, changeMask, newStateValue));
		}
		else if (xmlStrEqual (action->name, (xmlChar *) "command"))
		{
			if (action->children == NULL)
				throw XmlEmptyNode (action);
			
			commandName = std::string ((char *) action->children->content);
			stateCommands.push_back (new StateChangeCommand (deviceName, changeMask, newStateValue, commandName));
		}
		else if (xmlStrEqual (action->name, (xmlChar *) "email"))
		{
			StateChangeEmail *email = new StateChangeEmail (deviceName, changeMask, newStateValue);
			email->parse (action);
			// add to, subject, body,..
			stateCommands.push_back (email);
		}
		else
		{
			throw XmlUnexpectedNode (action);
		}
	}
}

void Events::parseValue (xmlNodePtr event, std::string deviceName)
{
	std::string condition;

	xmlAttrPtr valueName = xmlHasProp (event, (xmlChar *) "name");
	if (valueName == NULL)
		throw XmlMissingAttribute (event, "name");

	xmlAttrPtr cadencyPtr = xmlHasProp (event, (xmlChar *) "cadency");
	float cadency = -1;
	if (cadencyPtr != NULL)
		cadency = atof ((char *) cadencyPtr->children->content);

	xmlNodePtr action = event->children;
	for (; action != NULL; action = action->next)
	{
		if (xmlStrEqual (action->name, (xmlChar *) "record"))
		{
			valueCommands.push_back (new ValueChangeRecord (master, deviceName, std::string ((char *) valueName->children->content), cadency));
		}
		else if (xmlStrEqual (action->name, (xmlChar *) "command"))
		{
			valueCommands.push_back (new ValueChangeCommand (master, deviceName, std::string ((char *) valueName->children->content), cadency,
				std::string ((char *) action->children->content)));
		}
		else if (xmlStrEqual (action->name, (xmlChar *) "email"))
		{
			ValueChangeEmail *email = new ValueChangeEmail (master, deviceName, std::string ((char *) valueName->children->content), cadency);
			email->parse (action);
			// add to, subject, body,..
			valueCommands.push_back (email);
		}
		else
		{
			throw XmlUnexpectedNode (action);
		}
	}
}

void Events::parseHttp (xmlNodePtr ev)
{
	for (; ev; ev = ev->next)
	{
		if (xmlStrEqual (ev->name, (xmlChar *) "public"))
		{
			if (ev->children == NULL || ev->children->content == NULL)
				throw XmlMissingElement (ev, "content of public path");
			publicPaths.push_back (std::string ((char *) ev->children->content));
		}
		else if (xmlStrEqual (ev->name, (xmlChar *) "dir"))
		{
			if (ev->children != NULL)
				throw XmlError ("dir node must be empty");

			xmlAttrPtr path = xmlHasProp (ev, (xmlChar *) "path");
			if (path == NULL)
				throw XmlMissingAttribute (ev, "path");

			xmlAttrPtr to = xmlHasProp (ev, (xmlChar *) "to");
			if (to == NULL)
				throw XmlMissingAttribute (ev, "to");

			dirs.push_back (DirectoryMapping ((const char *) path->children->content, (const char *) to->children->content));
		}
		else if (xmlStrEqual (ev->name, (xmlChar *) "allsky"))
		{
			if (ev->children == NULL || ev->children->content == NULL)
				throw XmlMissingElement (ev, "content of allsky path");
			allskyPaths.push_back (std::string ((char *) ev->children->content));
		}
		else
		{
			throw XmlUnexpectedNode (ev);
		}
	}
}

void Events::parseEvents (xmlNodePtr ev)
{
	for (; ev; ev = ev->next)
	{
		if (xmlStrEqual (ev->name, (xmlChar *) "device"))
		{
			// parse it...
			std::string deviceName;
			std::string commandName;

			// does not have name..
			xmlAttrPtr attrname = xmlHasProp (ev, (xmlChar *) "name");
			if (attrname == NULL)
			{
				throw XmlMissingAttribute (ev, "name");
			}
			deviceName = std::string ((char *) attrname->children->content);

			for (xmlNodePtr event = ev->children; event != NULL; event = event->next)
			{
				// switch on action
				if (xmlStrEqual (event->name, (xmlChar *) "state"))
				{
						parseState (event, deviceName);
				}
				else if (xmlStrEqual (event->name, (xmlChar *) "value"))
				{
						parseValue (event, deviceName);
				}
				else
				{
					throw XmlUnexpectedNode (event);
				}
			}
		}
		else
		{
			throw XmlUnexpectedNode (ev);
		}
	}
}

void Events::load (const char *file)
{
	stateCommands.clear ();
	valueCommands.clear ();
	publicPaths.clear ();
	allskyPaths.clear ();

	xmlDoc *doc = NULL;
	xmlNodePtr root_element = NULL;

	LIBXML_TEST_VERSION

	xmlLineNumbersDefault (1);

	doc = xmlReadFile (file, NULL, XML_PARSE_NOBLANKS);
	if (doc == NULL)
	{
		logStream (MESSAGE_ERROR) << "cannot parse XML file " << file << sendLog;
		return;
	}

	root_element = xmlDocGetRootElement (doc);

	if (strcmp ((const char *) root_element->name, "config"))
		throw XmlUnexpectedNode (root_element);

	// traverse triggers..
	xmlNodePtr ev = root_element->children;
	if (ev == NULL)
	{
		logStream (MESSAGE_WARNING) << "no device specified" << sendLog;
		return;
	}
	for (; ev; ev = ev->next)
	{
		if (xmlStrEqual (ev->name, (xmlChar *) "http"))
		{
			parseHttp (ev->children);
		}
		else if (xmlStrEqual (ev->name, (xmlChar *) "events"))
		{
			parseEvents (ev->children);
		}
		else
		{
			throw XmlUnexpectedNode (ev);
		}
	}

	xmlFreeDoc (doc);
	xmlCleanupParser ();
}

bool Events::isPublic (std::string path)
{
	for (std::vector <std::string>::iterator iter = publicPaths.begin (); iter != publicPaths.end (); iter++)
	{
		// ends with *
		int l = iter->length () - 1;
		if ((*iter)[l] == '*')
		{
			if (path.substr (0, l - 1) == iter->substr (0, l - 1))
				return true;
		}
		else
		{
			if (path == *iter)
				return true;
		}
	}
	return false;
}
