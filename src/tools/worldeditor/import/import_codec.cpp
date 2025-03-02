/******************************************************************************
BigWorld Technology
Copyright BigWorld Pty, Ltd.
All Rights Reserved. Commercial in confidence.

WARNING: This computer program is protected by copyright law and international
treaties. Unauthorized use, reproduction or distribution of this program, or
any portion of this program, may result in the imposition of civil and
criminal penalties as provided by law.
******************************************************************************/

#include "pch.hpp"
#include "worldeditor/import/import_codec.hpp"


/*virtual*/ ImportCodec::~ImportCodec()
{
}


/*virtual*/ bool ImportCodec::canLoad() const
{
    return false;
}


/*virtual*/ bool ImportCodec::canSave() const
{
    return false;
}
