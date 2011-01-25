/*
 * Plan management application.
 * Copyright (C) 2005-2010 Petr Kubanek <petr@kubanek.net>
 * Copyright (C) 2011 Petr Kubanek, Institute of Physics <kubanek@fzu.cz>
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

#include "../utilsdb/rts2appdb.h"
#include "../utilsdb/plan.h"
#include "../utilsdb/planset.h"
#include "../utils/libnova_cpp.h"
#include "../utils/rts2config.h"

#include <signal.h>
#include <iostream>

#include <list>

#define OPT_DUMP        OPT_LOCAL + 701
#define OPT_LOAD        OPT_LOCAL + 702
#define OPT_GENERATE    OPT_LOCAL + 703
#define OPT_COPY        OPT_LOCAL + 704
#define OPT_DELETE      OPT_LOCAL + 705

class Rts2PlanApp:public Rts2AppDb
{
	public:
		Rts2PlanApp (int in_argc, char **in_argv);
		virtual ~ Rts2PlanApp (void);

		virtual int doProcessing ();
	protected:
		virtual int processOption (int in_opt);
	private:
		enum { NO_OP, OP_DUMP, OP_LOAD, OP_GENERATE, OP_COPY, OP_DELETE } operation;
		int dumpPlan ();
		int loadPlan ();
		int generatePlan ();
		int copyPlan ();
		int deletePlan ();

		double JD;
		std::list <int> ids;
};

Rts2PlanApp::Rts2PlanApp (int in_argc, char **in_argv):Rts2AppDb (in_argc, in_argv)
{
	Rts2Config *config;
	config = Rts2Config::instance ();

	JD = ln_get_julian_from_sys ();

	operation = NO_OP;

	addOption ('n', NULL, 1, "work with this night");
	addOption (OPT_DUMP, "dump", 0, "dump plan to standart output");
	addOption (OPT_LOAD, "load", 0, "load plan from standart input");
	addOption (OPT_GENERATE, "generate", 0, "generate plan based on targets");
	addOption (OPT_COPY, "copy", 1, "copy plan to given night (from night given by -n)");
	addOption (OPT_DELETE, "delete", 1, "delete plan with plan ID given as parameter");
}

Rts2PlanApp::~Rts2PlanApp (void)
{
}

int Rts2PlanApp::dumpPlan ()
{
	Rts2Night night = Rts2Night (JD, Rts2Config::instance ()->getObserver ());
	rts2db::PlanSet *plan_set = new rts2db::PlanSet (night.getFrom (), night.getTo ());
	plan_set->load ();
	std::cout << "Night " << night << std::endl;
	std::cout << (*plan_set) << std::endl;
	delete plan_set;
	return 0;
}

int Rts2PlanApp::loadPlan ()
{
	while (1)
	{
		rts2db::Plan plan;
		int ret;
		std::cin >> plan;
		if (std::cin.fail ())
			break;
		ret = plan.save ();
		if (ret)
			std::cerr << "Error loading plan" << std::endl;
	}
	return 0;
}

int Rts2PlanApp::generatePlan ()
{
	return -1;
}

int Rts2PlanApp::copyPlan ()
{
	return -1;
}

int Rts2PlanApp::deletePlan ()
{
	for (std::list <int>::iterator iter = ids.begin (); iter != ids.end (); iter++)
	{
		rts2db::Plan p (*iter);
		if (p.del ())
		{
			logStream (MESSAGE_ERROR) << "cannot delete plan with ID " << (*iter) << sendLog;
			return -1;	
		}
	}
	return 0;
}

int Rts2PlanApp::processOption (int in_opt)
{
	int ret;
	switch (in_opt)
	{
		case 'n':
			ret = parseDate (optarg, JD);
			if (ret)
				return ret;
			break;
		case OPT_DUMP:
			if (operation != NO_OP)
				return -1;
			operation = OP_DUMP;
			break;
		case OPT_LOAD:
			if (operation != NO_OP)
				return -1;
			operation = OP_LOAD;
			break;
		case OPT_GENERATE:
			if (operation != NO_OP)
				return -1;
			operation = OP_GENERATE;
			break;
		case OPT_COPY:
			if (operation != NO_OP)
				return -1;
			operation = OP_COPY;
			break;
		case OPT_DELETE:
			if (operation != NO_OP)
				return -1;
			operation = OP_DELETE;
			ids.push_back (atoi (optarg));
			break;
		default:
			return Rts2AppDb::processOption (in_opt);
	}
	return 0;
}

int Rts2PlanApp::doProcessing ()
{
	switch (operation)
	{
		case NO_OP:
			std::cout << "Please select mode of operation.." << std::endl;
			// interactive..
			return 0;
		case OP_DUMP:
			return dumpPlan ();
		case OP_LOAD:
			return loadPlan ();
		case OP_GENERATE:
			return generatePlan ();
		case OP_COPY:
			return copyPlan ();
		case OP_DELETE:
			return deletePlan ();
	}
	return -1;
}

int main (int argc, char **argv)
{
	Rts2PlanApp app = Rts2PlanApp (argc, argv);
	return app.run ();
}