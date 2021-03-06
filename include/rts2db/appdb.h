/* 
 * Application with database connection.
 * Copyright (C) 2004-2008 Petr Kubanek <petr@kubanek.net>
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

#ifndef __RTS2_APPDB__
#define __RTS2_APPDB__

#include <list>
#include <ostream>

#include "sqlcolumn.h"
#include "cliapp.h"
#include "timestamp.h"

namespace rts2db {

// for SQL tables display on console..
//
class SqlQuery
{
	public:
		SqlQuery (const char *in_from);
		virtual ~ SqlQuery (void);

		void addColumn (SqlColumn * add_column);
		void addColumn (const char *in_sql, const char *in_name, int in_order = UNORDER);
		void addWhere (const char *in_where);
		char *genSql ();
		void display ();

	private:
		std::list < SqlColumn * >columns;
		char *where;
		char *sql;
		char *from;
		void displayMinusPlusLine ();
};

class AppDb:public rts2core::CliApp
{
	public:
		AppDb (int argc, char **argv);
		virtual ~ AppDb (void);

		virtual int init ();

	protected:
		virtual int processOption (int in_opt);
		/**
		 * Called for query if the application really needs database access.
		 *
		 * @return true when the application needs database access.
		 */
		virtual bool doInitDB ();
		virtual void afterProcessing ();
	private:
		char *connectString;
		char *configFile;

		int initDB ();
};

}
#endif							 /* !__RTS2_APPDB__ */
