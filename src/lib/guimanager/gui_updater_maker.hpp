/******************************************************************************
BigWorld Technology
Copyright BigWorld Pty, Ltd.
All Rights Reserved. Commercial in confidence.

WARNING: This computer program is protected by copyright law and international
treaties. Unauthorized use, reproduction or distribution of this program, or
any portion of this program, may result in the imposition of civil and
criminal penalties as provided by law.
******************************************************************************/

#ifndef GUI_UPDATER_MAKER_HPP__
#define GUI_UPDATER_MAKER_HPP__


#include "gui_manager.hpp"


BEGIN_GUI_NAMESPACE


template< typename T, int index = 0 >
struct UpdaterMaker: public Updater
{
	unsigned int (T::*func_)( ItemPtr item );
	UpdaterMaker( const std::string& name, unsigned int (T::*func)( ItemPtr item ) )
		: func_( func )
	{
		if (GUI::Manager::pInstance())
		{
			GUI::Manager::instance().cppFunctor().set( name, this );
		}
	}
	~UpdaterMaker()
	{
		if (GUI::Manager::pInstance())
		{
			GUI::Manager::instance().cppFunctor().remove( this );
		}
	}
	virtual unsigned int update( ItemPtr item )
	{
		return ( ( ( T* )this ) ->* func_ )( item );
	}
};


END_GUI_NAMESPACE


#endif //GUI_UPDATER_MAKER_HPP__
