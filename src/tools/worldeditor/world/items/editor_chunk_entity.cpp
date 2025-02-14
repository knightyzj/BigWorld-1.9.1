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
#include "worldeditor/world/items/editor_chunk_entity.hpp"
#include "worldeditor/world/items/editor_chunk_substance.ipp"
#include "worldeditor/world/items/editor_chunk_station.hpp"
#include "worldeditor/world/items/editor_chunk_link.hpp"
#include "worldeditor/world/editor_chunk_item_linker.hpp"
#include "worldeditor/world/world_manager.hpp"
#include "worldeditor/world/editor_entity_proxy.hpp"
#include "worldeditor/world/entity_link_proxy.hpp"
#include "worldeditor/world/entity_property_parser.hpp"
#include "worldeditor/world/editor_chunk.hpp"
#include "worldeditor/world/editor_chunk_item_linker_manager.hpp"
#include "worldeditor/editor/item_editor.hpp"
#include "worldeditor/editor/item_properties.hpp"
#include "worldeditor/editor/editor_property_manager.hpp"
#include "appmgr/options.hpp"
#include "model/super_model.hpp"
#include "romp/geometrics.hpp"
#include "chunk/chunk_model.hpp"
#include "chunk/chunk_manager.hpp"
#include "entitydef/data_description.hpp"
#include "entitydef/entity_description.hpp"
#include "entitydef/data_types.hpp"
#include "entitydef/constants.hpp"
#include "resmgr/bwresource.hpp"
#include "resmgr/xml_section.hpp"
#include "resmgr/string_provider.hpp"
#include "resmgr/auto_config.hpp"
#include "gizmo/link_property.hpp"
#include "cstdmf/debug.hpp"

#if UMBRA_ENABLE
#include <umbraModel.hpp>
#include <umbraObject.hpp>
#include "chunk/chunk_umbra.hpp"
#endif

DECLARE_DEBUG_COMPONENT2( "Editor", 0 )


std::vector<EditorChunkEntity*> EditorChunkEntity::s_dirtyModelEntities_;
SimpleMutex EditorChunkEntity::s_dirtyModelMutex_;
SimpleMutex EditorChunkEntity::s_loadingModelMutex_;


static AutoConfigString s_notFoundModel( "system/notFoundModel" );

static std::string s_defaultModel = "resources/models/entity.model";


// Link to the UalEntityProvider, so entities get listed in the Asset Locator
// this token is defined in ual_entity_provider.cpp
extern int UalEntityProv_token;
static int total = UalEntityProv_token;


// -----------------------------------------------------------------------------
// Section: EditorEntityType
// -----------------------------------------------------------------------------
EditorEntityType * EditorEntityType::s_instance_ = NULL;

void EditorEntityType::startup()
{
	MF_ASSERT(!s_instance_);
	s_instance_ = new EditorEntityType();
}

void EditorEntityType::shutdown()
{
	MF_ASSERT(s_instance_);
	delete s_instance_;
	s_instance_ = NULL;
}

static void initEditorPropertyTypes();

EditorEntityType::EditorEntityType()
{
	initEditorPropertyTypes();
	// load normal entities
	eMap_.parse( BWResource::openSection(
		EntityDef::Constants::entitiesFile() ) );
	// load markers with properties that are pretending to be entities
	DataSectionPtr markers = BWResource::openSection(
		EntityDef::Constants::markerCategoriesFile() );
	if (markers)
	{
		for (DataSectionIterator mit = markers->begin(); mit != markers->end(); mit++)
		{
			DataSectionPtr marker = *mit;
			// test to see whether an entity
			DataSectionPtr mlevel = marker->openSection( "level" );
			if (mlevel) continue;

			DataSectionPtr mprops = marker->openSection( "Properties" );
			if (!mprops) continue;

			std::string category = marker->sectionName();

			EntityDescription * pED = new EntityDescription();
			if (!pED->parse( category, marker ))
			{
				ERROR_MSG( "Could not parse marker %s as entity\n", category.c_str() );
				continue;
			}
			for (uint i = 0; i < pED->propertyCount(); i++)
				pED->property( i )->editable( true );
			mMap_[ category ] = pED;
		}
	}

	// load the editor entity scripts
	INFO_MSG( "EditorEntityType constructor - Importing editor entity scripts\n" );
	DataSectionPtr edScripts = BWResource::openSection(
		EntityDef::Constants::entitiesEditorPath() );

	if( edScripts )
	{
		for (EntityDescriptionMap::DescriptionMap::const_iterator it = eMap_.begin();
			it != eMap_.end(); it++)
		{
			std::string name = (*it).first;

			if ( edScripts->openSection( name + ".py" ) == NULL )
			{
				INFO_MSG( "EditorEntityType - no editor script found for %s\n", name.c_str() );
				continue;
			}

			// class name and module name are the same
			PyObject * pModule = PyImport_ImportModule( const_cast<char *>( name.c_str() ) );
			if (PyErr_Occurred())
			{
				ERROR_MSG( "EditorEntityType - fail to import editor script %s\n", name.c_str() );
				PyErr_Print();
				continue;
			}

			MF_ASSERT(pModule);

			PyObject * pyClass = PyObject_CallMethod( pModule, const_cast<char *>( name.c_str() ), "" );
			Py_XDECREF( pModule );

			if (PyErr_Occurred())
			{
				ERROR_MSG( "EditorEntityType - fail to open editor script %s\n", name.c_str() );
				PyErr_Print();
				continue;
			}

			MF_ASSERT(pyClass);

			pyClasses_[ name ] = pyClass;
		}
	}
}

const EntityDescription * EditorEntityType::get( const std::string & name )
{
	// try it as an entity
	EntityTypeID index = 0;
	if (eMap_.nameToIndex( name, index ))
		return &eMap_.entityDescription( index );

	// try it as a marker
	if (mMap_[ name ] != NULL) return mMap_[ name ];

	return NULL;
}

PyObject * EditorEntityType::getPyClass( const std::string & name )
{
	std::map<std::string, PyObject *>::iterator mit = pyClasses_.find( name );
	if (mit != pyClasses_.end())
	{
		return mit->second;
	}

	return NULL;
}

EditorEntityType& EditorEntityType::instance()
{
	MF_ASSERT(s_instance_);
	return *s_instance_;
}



/*
// required by entity description, for a method we never call.
#include "network/mercury.hpp"
namespace Mercury
{
	int Bundle::size() const { return 0; }
};
*/


// -----------------------------------------------------------------------------
// Section: SetupEditorPropertyTypes
// -----------------------------------------------------------------------------

class SetupEditorPropertyTypes
{
public:
	//TODO: remove PATROL_PATH when it is no longer needed
	SetupEditorPropertyTypes() :
		patrolPathDataType( "PATROL_PATH" )
	{
	}

	// We extend string rather that PatrolPathDataType, as we only want to
	// manipulate the resource it points to, not the data
	class EditorPatrolPathDataType : public StringDataType
	{
	public:
		EditorPatrolPathDataType( MetaDataType * pMetaType ) : StringDataType( pMetaType ) { }

		GeneralProperty * createEditorProperty( const std::string& name,
			ChunkItem* editorChunkEntity, int editorEntityPropertyId )
		{
            return
                new LinkProperty
                (
                    name,
                    new EntityLinkProxy( (EditorChunkEntity*) editorChunkEntity ),
                    NULL // use the selection's matrix
                );
		}
	};

private:
	SimpleMetaDataType< EditorPatrolPathDataType > patrolPathDataType;

};

static void initEditorPropertyTypes()
{
	// Used to ensure we call the once, after the normal static vars have been
	// inited
	static SetupEditorPropertyTypes setup;
}




// -----------------------------------------------------------------------------
// Section: EditorChunkEntity
// -----------------------------------------------------------------------------

static void initEditorPropertyTypes();

/**
 *	Constructor.
 */
EditorChunkEntity::EditorChunkEntity() :
	pType_( NULL ),
	transform_( Matrix::identity ),
	transformLoaded_( false ),
	pDict_( NULL ),
	pyClass_( NULL ),
	guid_(UniqueID::generate()),
	surrogateMarker_( false ),
	model_( NULL ),
	firstLinkFound_( false ),
	loadBackgroundTask_( NULL )
{
	initEditorPropertyTypes();
	pChunkItemLinker_ = new EditorChunkItemLinkable(this, guid_, &propHelper_);
}

/**
 *	Destructor.
 */
EditorChunkEntity::~EditorChunkEntity()
{
	(pChunkItemLinker_) ? delete pChunkItemLinker_ : 0;
	waitDoneLoading();
	removeFromDirtyList();
	Py_XDECREF( pDict_ );
}


bool EditorChunkEntity::initType( const std::string type, std::string* errorString )
{
	if ( pType_ != NULL )
		return true;

	pType_ = EditorEntityType::instance().get( type );

	if (!pType_)
	{
		if (surrogateMarker_)
			return true;

		std::string err = "No definition for entity type '" + type + "'";
		if ( errorString )
		{
			*errorString = err;
		}
		else
		{
			ERROR_MSG( "EditorChunkEntity::load - %s\n", err.c_str() );
		}
		return false;
	}

	return true;
}


/**
 *	This method returns a dictionary containing internal attributes of the
 *	Entity.
 *
 *	@return		A python dictionary containing chunk id, guid, position, etc.
 */
PyObject* EditorChunkEntity::infoDict() const
{
	PyObject* dict = PyDict_New();

	// add general item data
	PyDict_SetItemString( dict, "chunk", Py_BuildValue( "s", chunkItemLinker()->getOutsideChunkId().c_str() ) );
	PyDict_SetItemString( dict, "guid", Py_BuildValue( "s", guid_.toString().c_str() ) );
	PyDict_SetItemString( dict, "type", Py_BuildValue( "s", pType_->name().c_str() ) );
	Vector3 pos = transform_.applyToOrigin();
	pos = chunk()->transform().applyPoint( pos );
	PyDict_SetItemString( dict, "position", Py_BuildValue( "(fff)", pos.x, pos.y, pos.z ) );

	// add back links
	PyObjectPtr backLinks( PyTuple_New( this->chunkItemLinker()->getBackLinksCount() ), PyObjectPtr::STEAL_REFERENCE );
	int ti = 0;
	for( EditorChunkItemLinkable::Links::const_iterator i = chunkItemLinker()->getBackLinksBegin(); i != chunkItemLinker()->getBackLinksEnd(); ++i, ++ti )
	{
		PyTuple_SetItem( backLinks.getObject(), ti,
			Py_BuildValue( "(ss)", (*i).UID_.toString().c_str(), (*i).CID_.c_str() ) );
	}
	PyDict_SetItemString( dict, "backLinks", backLinks.getObject() );

	// add the properties
	PyDict_SetItemString( dict, "properties", pDict_ );

	return dict;
}


/**
 *	This method asks the entity's editor script to find out if this entity can
 *	be linked to the User Data Object.
 *
 *	@param propName		Name of the link property in this entity.
 *	@param otherInfo	User Data Object to link to.
 *	@return				true if this entity can link to the other.
 */
bool EditorChunkEntity::canLinkTo( const std::string& propName, PyObject* otherInfo ) const
{
	if ( !pyClass_ )
		return true;

	PyObject* thisInfo = infoDict();

	PyObject * result = Script::ask(
		PyObject_GetAttrString( pyClass_, "canLink" ),
		Py_BuildValue( "(sOO)", propName.c_str(), thisInfo, otherInfo ),
		"EditorChunkEntity::canLinkTo: ",
		true /* ok if function NULL */ );

	Py_DECREF( thisInfo );

	if ( !result || !PyBool_Check( result ) )
		return true;

	return result == Py_True;
}


/**
 *	Our load method. We can't call (or reference) the base class's method
 *	because it would not compile (chunk item has no load method)
 */
bool EditorChunkEntity::load( DataSectionPtr pSection, Chunk * pChunk, std::string* errorString )
{
	edCommonLoad( pSection );

	// read known properties
	model_ = Model::get( s_defaultModel );

	pOwnSect_ = pSection;

	std::string typeName = pOwnSect_->readString( "type" );

	if ( !initType( typeName, errorString ) )
	{
		ModelPtr badModel = Model::get( s_notFoundModel.value() );
		MF_ASSERT( badModel != NULL );
		if ( badModel != NULL )
			model_ = badModel;
		WorldManager::instance().addError(
			pChunk, this, "Couldn't load entity: %s", typeName.c_str() );
		pOriginalSect_ = pSection;
	}

	if ( Moo::g_renderThread )
	{
		// If loading from the main thread, load straight away
		edLoad( pOwnSect_ );
	}
	return true;
}

void EditorChunkEntity::edMainThreadLoad()
{
	// have to load this in the main thread to avoid multi-thread issues with
	// some python calls/objects in edLoad
	edLoad( pOwnSect_, false );
}

bool EditorChunkEntity::edLoad( const std::string type, DataSectionPtr pSection, std::string* errorString )
{
	if ( pDict_ != NULL )
		return true; // already initialised

	if ( !initType( type, errorString ) )
		return false;

	return edLoad( pSection );
}

bool EditorChunkEntity::edLoad( DataSectionPtr pSection, bool loadTransform )
{
	// get rid of any current state
	Py_XDECREF( pDict_ );
	pDict_ = NULL;

	std::string idStr = pSection->readString( "guid" );
	if (!idStr.empty()){
		guid_ = UniqueID(idStr);
	} else{
		guid_ = UniqueID::generate();
	}
	chunkItemLinker()->guid(guid_);

	if (!surrogateMarker_)
	{
		if (loadTransform || !transformLoaded_)
		{
			transform_ = pSection->readMatrix34( "transform", Matrix::identity );
			transformLoaded_ = true;
		}

		DataSectionPtr clientOnlyDS = pSection->openSection( "clientOnly" );
		if ( !clientOnlyDS )
		{
			// If the clientOnly section hasn't been set, set it according to
			// the scripts the entity has.
			if ( pType_ != NULL && !pType_->hasBaseScript() && !pType_->hasCellScript() )
				clientOnly_ = true;
			else
				clientOnly_ = false;
		}
		else
		{
			clientOnly_ = clientOnlyDS->asBool( false );
		}
	}
	if ( pSection->findChild( "patrolPathNode" ) != NULL )
		patrolListNode_ = pSection->readString("patrolPathNode");
	else
		patrolListNode_ = pSection->readString("properties/patrolPathNode");

	// Load in the back links
	chunkItemLinker()->loadBackLinks(pSection);

	if (!pType_ )
	{
		if (surrogateMarker_)
			return true; // early return if it's a marker

		markModelDirty();
		return false;
	}

	// read item properties (also from parents)
	pDict_ = PyDict_New();
	DataSectionPtr propertiesSection = pSection->openSection( "properties" );
	std::vector<bool> usingDefault( pType_->propertyCount(), true );
	for (uint i=0; i < pType_->propertyCount(); i++)
	{
		DataDescription * pDD = pType_->property(i);

		if (!pDD->editable())
			continue;

		DataSectionPtr	pSubSection;

		PyObjectPtr pValue = NULL;

		// Can we get it from the DataSection?
		if (propertiesSection && (pSubSection = propertiesSection->openSection( pDD->name() )))
		{
			// TODO: support for UserDataType
			pValue = pDD->createFromSection( pSubSection );
			PyErr_Clear();
		}

		// ok, resort to the default then
		usingDefault[i] = ( !pValue );
		if (!pValue)
		{
			pValue = pDD->pInitialValue();
			if (pDD->dataType()->typeName().find( "ARRAY:" ) != std::string::npos)
			{
				// Can't use initial value for sequences/arrays because
				// pInitialValue is a shared object, and WE's arrays need a
				// new object it can modify (i.e. add array elements)
				pValue = pDD->dataType()->pDefaultValue();
			}
			else
			{
				// Using the shared pInitialValue, so increment refcount
				Py_INCREF( &*pValue );
			}
		}

		PyDict_SetItemString( pDict_,
			const_cast<char*>( pDD->name().c_str() ), &*pValue );
	}

	// record the links to other models
	recordBindingProps();

	// find the correct model
	markModelDirty();

	// find the reference to the editor python class
	std::string className = pType_->name();
	pyClass_ = EditorEntityType::instance().getPyClass( className );

	propHelper_.init(
		this,
		const_cast<EntityDescription*>( pType_ ),
		pDict_,
		new BWFunctor1<EditorChunkEntity, int>(
			this, &EditorChunkEntity::propertyChangedCallback ) );

	propHelper_.propUsingDefaults( usingDefault );

	return true;
}

void EditorChunkEntity::clearProperties()
{
	propHelper_.clearProperties();
}

void EditorChunkEntity::clearEditProps()
{
	propHelper_.clearEditProps( allowEdit_ );
}

void EditorChunkEntity::setEditProps( const std::list< std::string > & names )
{
	propHelper_.setEditProps( names, allowEdit_ );
}


/**
 *	Save any property changes to this data section
 */
void EditorChunkEntity::clearPropertySection()
{
	pType_ = NULL;

	if (!pOwnSect())
		return;

	propHelper_.clearPropertySection( pOwnSect() );
}


class UserDataType;

bool EditorChunkEntity::edSave( DataSectionPtr pSection )
{
	if (!edCommonSave( pSection ))
		return false;

	if (pType_ == NULL)
	{
		if (surrogateMarker_)
		{
			return true;
		}
		else if ( pOriginalSect_ )
		{
			// there was an error loading the entity type, so save the original
			// datasection but modify the transform.
			pSection->copy( pOriginalSect_ );
			pSection->writeMatrix34( "transform", transform_ );
			return true;
		}
		else
		{
			return false;
		}
	}

	// write the easy ones
	if (!surrogateMarker_)
	{
		// write the easy ones
		pSection->writeString( "guid", guid_.toString() );

		pSection->writeString( "type", pType_->name() );

		pSection->writeMatrix34( "transform", transform_ );

		pSection->writeBool( "clientOnly", clientOnly_ );

		pSection->delChild( "patrolPathNode" );
	}

	DataSectionPtr propertiesSection = pSection->openSection( "properties", true );
	propertiesSection->delChildren();

	// now write the properties from the dictionary
	for (uint i=0; i < pType_->propertyCount(); i++)
	{
		DataDescription * pDD = pType_->property(i);

		if (!pDD->editable())
			continue;

		PyObject * pValue = PyDict_GetItemString( pDict_,
			const_cast<char*>( pDD->name().c_str() ) );
		if (!pValue)
		{
			PyErr_Print();
			ERROR_MSG( "EditorChunkEntity::edSave: Failed to get prop %s\n",
				pDD->name().c_str() );
			continue;
		}

		if ( propHelper_.propUsingDefault( i ) )
		{
			propertiesSection->delChild( pDD->name() );
		}
		else
		{
			DataSectionPtr pDS = propertiesSection->openSection( pDD->name(), true );
			pDD->dataType()->addToSection( pValue, pDS );
		}
	}

	propertiesSection->writeString("patrolPathNode", patrolListNode_);

	// Delete backLinks sectionClear the links
	chunkItemLinker()->saveBackLinks(pSection);

	return true;
}


/**
 *  This is called when the entity is removed from, or added to a Chunk.  Here
 *  we delete the link, it gets recreated if necessary later on, and call the
 *  base class.
 */
/*virtual*/ void EditorChunkEntity::toss( Chunk * pChunk )
{
    if (link_ != NULL)
    {
        link_->startItem(NULL);
        link_->endItem(NULL);
        link_->toss(NULL);
        link_ = NULL;
    }

	// Remove references to links, i.e. break cyclic shared pointer use:
    if (pChunk)
	{
		// Call the base method first
		EditorChunkSubstance<ChunkItem>::toss( pChunk );

		// Add links for this nodes back links
		chunkItemLinker()->tossAdd();
	}
	else
	{
		// Add links for this nodes back links
		chunkItemLinker()->tossRemove();

		// Call the base method last
		EditorChunkSubstance<ChunkItem>::toss( pChunk );
	}
}


/*virtual*/ void EditorChunkEntity::edPreDelete()
{
	// Let linker know that we are being deleted.
	chunkItemLinker()->deleted();
	EditorChunkItem::edPreDelete();
}


/**
 *	This method returns false if this Entity is linked to UDO(s) in chunk(s)
 *	that is not locked for writing.
 *
 *  @return		true if the Entity can be deleted, false otherwise.
 */
/*virtual*/ bool EditorChunkEntity::edCanDelete()
{
	if ( !chunkItemLinker()->linkedChunksWriteable() )
	{
		ERROR_MSG( "Can't delete this entity (guid %s) because it's linked to chunks that are not locked for writing.\n",
			guid().toString().c_str() );
		return false;
	}
	return true;
}


/**
 *	Change our transform, temporarily or permanently
 */
bool EditorChunkEntity::edTransform( const Matrix & m, bool transient )
{
	// Update the status of transient_
	transient_ = transient;

	// it's permanent, so find out where we belong now
	Chunk * pOldChunk = pChunk_;
	Chunk * pNewChunk = this->edDropChunk( m.applyToOrigin() );
	if (pNewChunk == NULL) return false;

	// make sure the chunks aren't readonly, and also make sure that all
	// affected chunks are writeable if changing chunks.
	if (!EditorChunkCache::instance( *pOldChunk ).edIsWriteable() ||
		!EditorChunkCache::instance( *pNewChunk ).edIsWriteable() ||
		( pNewChunk != pOldChunk && !chunkItemLinker()->linkedChunksWriteable() ) )
	{
		return false;
	}

	// if this is only a temporary change, keep it in the same chunk
	if (transient)
	{
		transform_ = m;
		// update chunk links
		chunkItemLinker()->updateChunkLinks();
		this->syncInit();
		return true;
	}

	// make sure the chunks aren't readonly
	if (!EditorChunkCache::instance( *pOldChunk ).edIsWriteable()
		|| !EditorChunkCache::instance( *pNewChunk ).edIsWriteable())
		return false;

	// ok, accept the transform change then
	transform_.multiply( m, pOldChunk->transform() );
	transform_.postMultiply( pNewChunk->transformInverse() );

	// note that both affected chunks have seen changes
	WorldManager::instance().changedChunk( pOldChunk );
	WorldManager::instance().changedChunk( pNewChunk );

	// and move ourselves into the right chunk. we have to do this
	// even if it's the same chunk so the col scene gets recreated
	pOldChunk->delStaticItem( this );
	pNewChunk->addStaticItem( this );

	// update chunk links
	chunkItemLinker()->updateChunkLinks();
	this->syncInit();
	return true;
}


/**
 *	Get a description of this item
 */
std::string EditorChunkEntity::edDescription()
{
	std::string name = "<unknown>";
	if ( pType_ )
		name = pType_->name();
	else if ( pOriginalSect_ )
		name = pOriginalSect_->readString( "type" ) + " " + name;

	return L( "WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_CHUNK_ENTITY/ED_DESCRIPTION", name );
}

std::vector<std::string> EditorChunkEntity::edCommand( const std::string& path ) const
{
	EditorChunkEntity *myself =
		const_cast<EditorChunkEntity *>(this);

	return myself->propHelper()->command();
}

bool EditorChunkEntity::edExecuteCommand( const std::string& path, std::vector<std::string>::size_type index )
{
	PropertyIndex pi = propHelper()->commandIndex( index );

	DataDescription* pDD = propHelper()->pType()->property( pi.valueAt( 0 ) );
	if (!pDD->editable())
		return false;

	bool link = propHelper()->isUserDataObjectLink( pi.valueAt(0) );
	bool arrayLink = propHelper()->isUserDataObjectLinkArray( pi.valueAt(0) );
	if ( link || ( arrayLink && pi.count() > 1 ) )
	{
		PyObjectPtr ob( propHelper()->propGetPy( pi ), PyObjectPtr::STEAL_REFERENCE );
		std::string uniqueId = PyString_AsString( PyTuple_GetItem( ob.getObject(), 0 ) );
		std::string chunkId = PyString_AsString( PyTuple_GetItem( ob.getObject(), 1 ) );

		EditorChunkItemLinkable* pEcil = WorldManager::instance().linkerManager().forceLoad( uniqueId, chunkId );
		if ( pEcil &&
			EditorChunkCache::instance( *(pEcil->chunkItem()->chunk()) ).edIsWriteable() )
		{
			WorldManager::instance().linkerManager().deleteLink( chunkItemLinker(), pEcil, pi );

			propHelper()->resetSelUpdate( true );
			// Refresh item
			propHelper()->refreshItem();

			if ( link )
				UndoRedo::instance().barrier( L("WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_CHUNK_USER_DATA_OBJECT/UNDO_DEL_ITEM"), false );
			else
				UndoRedo::instance().barrier( L("WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_ENTITY_PROXY/UNDO_DEL_ARRAY_ITEM"), false );
		}
		else
			ERROR_MSG( "Could not delete link %s as the link does not exist or the chunk %s is locked\n",
				uniqueId.c_str(), chunkId.c_str() );
	}
	else if ( arrayLink )
	{
		PyObjectPtr ob( propHelper()->propGetPy( pi ), PyObjectPtr::STEAL_REFERENCE );

		SequenceDataType* dataType =
			static_cast<SequenceDataType*>( pDD->dataType() );
		ArrayPropertiesHelper propArray;
		propArray.init( this, &(dataType->getElemType()), ob.getObject());

		// Iterate through the array of links
		for(int j = 0; j < propArray.propCount(); j++)
		{
			PyObjectPtr link( propArray.propGetPy( j ), PyObjectPtr::STEAL_REFERENCE );
			std::string uniqueId = PyString_AsString( PyTuple_GetItem( link.getObject(), 0 ) );
			std::string chunkId = PyString_AsString( PyTuple_GetItem( link.getObject(), 1 ) );

			EditorChunkItemLinkable* pEcil = WorldManager::instance().linkerManager().forceLoad( uniqueId, chunkId );
			if ( pEcil &&
				EditorChunkCache::instance( *(pEcil->chunkItem()->chunk()) ).edIsWriteable() )
			{
				PropertyIndex piArray( pi.valueAt( 0 ) );
				piArray.append( j-- );
				WorldManager::instance().linkerManager().deleteLink( chunkItemLinker(), pEcil, piArray );
			}
			else
				ERROR_MSG( "Could not delete link %s as the link does not exist or the chunk %s is locked\n",
					uniqueId.c_str(), chunkId.c_str() );

		}
		propHelper()->resetSelUpdate( true );
		// Refresh item
		propHelper()->refreshItem();

		UndoRedo::instance().barrier( L("WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_ENTITY_PROXY/UNDO_CLEAR_ARRAY"), false );
	}

	return true;
}


bool EditorChunkEntity::edShouldDraw()
{
	if (ChunkItem::edShouldDraw() &&
		!!Options::getOptionInt( "render/gameObjects", 1 ) &&
		!!Options::getOptionInt( "render/gameObjects/drawEntities", 1 ))
		return true;
	else
		return false;
}


void EditorChunkEntity::patrolListNode(std::string const &id)
{
    patrolListNode_ = id;
}


std::string EditorChunkEntity::patrolListNode() const
{
    return patrolListNode_;
}


std::string EditorChunkEntity::patrolListGraphId() const
{
    int idx = patrolListPropIdx();
    if (idx == -1)
        return std::string();
    else
        return propHelper_.propGetString(idx);
}


int EditorChunkEntity::patrolListPropIdx() const
{
	if (pType_ == NULL)
		return -1;

	for (uint i=0; i < pType_->propertyCount(); i++)
	{
		DataDescription * pDD = pType_->property(i);
		if (pDD->editable() && pDD->dataType()->typeName() == "PATROL_PATH" )
		{
			return i;
		}
	}

	return -1;
}


void EditorChunkEntity::disconnectFromPatrolList()
{
    patrolListNode_.clear();
    int idx = patrolListPropIdx();
    if (idx != -1)
    {
        propHelper_.propSetString(idx, std::string());
    }
    if (link_ != NULL)
    {
        link_->toss(NULL);
        link_->startItem(NULL);
        link_->endItem(NULL);
        link_ = NULL;
    }
}


/**
 *	Relinks an entity to the node 'newNode' in the patrol path 'newGraph'
 *
 *  @param newGraph       New patrol path the entity should be linked to
 *  @param newNode        New patrol path node the entity should be linked to
 *
 *  @return               true if the entity has a patrol list property
 */
bool EditorChunkEntity::patrolListRelink( const std::string& newGraph, const std::string& newNode )
{
    int idx = patrolListPropIdx();
    if (idx != -1)
    {
        propHelper_.propSetString( idx, newGraph );
        patrolListNode( newNode );
        edSave( pOwnSect_ );
        if ( pChunk_ != NULL )
            WorldManager::instance().changedChunk( pChunk_ );
		return true;
    }
	return false;
}



/**
 *	Add the properties of this chunk entity to the given editor
 */
bool EditorChunkEntity::edEdit( class ChunkItemEditor & editor )
{
	editor.addProperty( new StaticTextProperty(
		L( "WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_CHUNK_ENTITY/TYPE" ),
		new AccessorDataProxy< EditorChunkEntity, StringProxy >(
			this, "type",
			&EditorChunkEntity::typeGet,
			&EditorChunkEntity::typeSet ) ) );

	editor.addProperty( new StaticTextProperty( "guid",
		new AccessorDataProxy< EditorChunkEntity, StringProxy >(
			this, "guid",
			&EditorChunkEntity::idGet,
			&EditorChunkEntity::idSet ) ) );

	MatrixProxy * pMP = new ChunkItemMatrix( this );
	editor.addProperty( new ChunkItemPositionProperty(
		L( "WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_CHUNK_ENTITY/POSITION" ), pMP, this ) );
	editor.addProperty( new GenRotationProperty(
		L( "WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_CHUNK_ENTITY/DIRECTION" ), pMP ) );

	editor.addProperty( new GenBoolProperty( L( "WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_CHUNK_ENTITY/CLIENT_ONLY" ),
		new AccessorDataProxy< EditorChunkEntity, BoolProxy >(
			this, "clientOnly",
			&EditorChunkEntity::clientOnlyGet,
			&EditorChunkEntity::clientOnlySet ) ) );

	editor.addProperty( new StaticTextProperty( L( "WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_CHUNK_ENTITY/LAST_SCRIPT_ERROR" ),
		new AccessorDataProxy<EditorChunkEntity,StringProxy>(
			this, "READONLY",
			&EditorChunkEntity::lseGet,
			&EditorChunkEntity::lseSet ) ) );

    editor.addProperty( new StaticTextProperty( L( "WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_CHUNK_ENTITY/GRAPH" ),
        new AccessorDataProxy<EditorChunkEntity,StringProxy>(
            this, "READONLY",
            &EditorChunkEntity::graphId,
            &EditorChunkEntity::graphId ) ) );

    editor.addProperty( new StaticTextProperty( L( "WORLDEDITOR/WORLDEDITOR/CHUNK/EDITOR_CHUNK_ENTITY/NODE" ),
        new AccessorDataProxy<EditorChunkEntity,StringProxy>(
            this, "READONLY",
            &EditorChunkEntity::nodeId,
            &EditorChunkEntity::nodeId ) ) );

	return edEditProperties( editor, pMP );
}


bool EditorChunkEntity::edEditProperties( class ChunkItemEditor & editor, MatrixProxy * pMP )
{
//	return true;

	recordBindingProps();

	if (pType_ == NULL)
	{
		if (surrogateMarker_)
		{
			return true;
		}
		else
		{
			ERROR_MSG( "EditorChunkEntity::edEdit - no properties for entity!\n" );
			return false;
		}
	}

	bool hasActions = false;

	// this flag is used to make the first link always show by default
	firstLinkFound_ = false;

	// ok, now finally add in all the entity properties!
	for (uint i=0; i < pType_->propertyCount(); i++)
	{
		DataDescription * pDD = pType_->property(i);

		if (!pDD->editable())
			continue;

		if (surrogateMarker_ && (allowEdit_.capacity() != 0) && !allowEdit_[i])
			continue;

		GeneralProperty* prop = pDD->dataType()->createEditorProperty(
			pDD->name(), this, i );

		if (prop)
		{
			editor.addProperty( prop );
			continue;
		}

		EntityPropertyParserPtr parser = EntityPropertyParser::create(
			pyClass_, pDD->name(), pDD->dataType(), pDD->widget() );
		if ( parser )
		{
			prop = parser->createProperty(
				&propHelper_, i, pDD->name(), pDD->dataType(), pDD->widget(), pMP );
		}
		if ( prop )
		{
			editor.addProperty( prop );
		}
		else
		{
			// TODO: Should probably make this read-only. It may not work if the
			// Python object does not have a repr that can be eval'ed.
			// treat everything else as a generic Python property
			editor.addProperty( new PythonProperty( pDD->name(),
				new EntityPythonProxy( &propHelper_, i ) ) );
		}
	}

	return true;
}


void EditorChunkEntity::propertyChangedCallback( int index )
{
	markModelDirty();
}


void EditorChunkEntity::recordBindingProps()
{
//	return;

	bindingProps_.clear();

	if (pType_ == NULL)
		return;

	for (uint i=0; i < pType_->propertyCount(); i++)
	{
		DataDescription * pDD = pType_->property(i);

		if (!pDD->editable())
			continue;

		if (pDD->dataType()->typeName().find( "ARRAY:" ) != std::string::npos)
		{
			PyObjectPtr actions( propHelper_.propGetPy(i), PyObjectPtr::STEAL_REFERENCE );

			const int listSize = PyList_Size( actions.getObject() );
			for (int listIndex = 0; listIndex < listSize; listIndex++)
			{
				PyObject * action = PyList_GetItem( actions.getObject(), listIndex );

				PyInstanceObject * pInst = (PyInstanceObject *)(action);
				PyClassObject * pClass = (PyClassObject *)(pInst->in_class);
				std::string userClassName = PyString_AsString( pClass->cl_name );

				bool argsAdded = false;
				PyObject * dict = PyObject_GetAttrString( action, "ARGS" );
				MF_ASSERT( dict );
				PyObject * items = PyDict_Items( dict );
				Py_XDECREF( dict );

				for (int j = 0; j < PyList_Size( items ); j++)
				{
					PyObject * item = PyList_GetItem( items, j );
					std::string argName = PyString_AsString( PyTuple_GetItem( item, 0 ) );
					std::string argType = PyString_AsString( PyTuple_GetItem( item, 1 ) );

					if (argType == "ENTITY_ID")
						bindingProps_.push_back( BindingProperty(action, argName) );
				}
				Py_XDECREF( items );
			}
		}
	}
}


/**
 *  If we've got a patrolList property, and it's connected to a station graph,
 *  find the connected node.  This is either the explicitly named node, or
 *  the closest node.
 */
EditorChunkStationNode* EditorChunkEntity::connectedNode()
{
    std::string graphName = patrolListGraphId();

    if (graphName.empty())
        return NULL;

	UniqueID graphNameID(graphName);
	StationGraph* graph = StationGraph::getGraph( graphNameID );
    if (graph == NULL)
        return NULL;

    // If there is an explicitly named node, and it's in this graph
    // then return the node:
    if (!patrolListNode_.empty())
    {
        EditorChunkStationNode *node =
            (EditorChunkStationNode *)graph->getNode(patrolListNode_);
        if (node != NULL && !node->deleted())
            return node;
    }

    // Find the nearest node:
    std::vector<ChunkStationNode*> nodes = graph->getAllNodes();
	if (nodes.empty())
		return NULL;

	EditorChunkStationNode *closestNode = NULL;

	float closestDistSq = FLT_MAX;

	Vector3 from = chunk()->transform().applyPoint( edTransform().applyToOrigin() )
		+ Vector3(0.f, 1.f, 0.f);

	std::vector<ChunkStationNode*>::iterator i = nodes.begin();
	for (; i != nodes.end(); ++i)
	{
		EditorChunkStationNode* dest = static_cast<EditorChunkStationNode*>(*i);

		if (!dest->chunk())
			continue;

		Vector3 to = dest->chunk()->transform().applyPoint( dest->edTransform().applyToOrigin() )
			+ Vector3(0.f, 0.1f, 0.f);

		float len = (to - from).lengthSquared();
		if (len < closestDistSq)
		{
			closestNode = dest;
			closestDistSq = len;
		}
	}

	return closestNode;
}


void EditorChunkEntity::draw()
{
	if (edShouldDraw())
	{
		EditorChunkSubstance<ChunkItem>::draw();

		EditorChunkStationNode* station = connectedNode();
		if (station != NULL && !WorldManager::instance().drawSelection() )
		{
            if (link_ == NULL)
            {
                link_ = new EditorChunkLink();
                link_->chunk(chunk());
                link_->startItem(this);
                link_->endItem(station);
            }
			link_->draw();
		}
	}
}


/*virtual*/ void EditorChunkEntity::tick(float dtime)
{
    EditorChunkSubstance<ChunkItem>::tick(dtime);

    EditorChunkStationNode* station = connectedNode();
    if (station != NULL && link_ != NULL)
    {
        link_->startItem(this);
        link_->endItem(station);
        link_->tick(dtime);
    }
}


std::string EditorChunkEntity::typeGet() const
{
	if ( pType_ == NULL )
	{
		if ( pOriginalSect_ )
			return pOriginalSect_->readString( "type" );
		else
			return "";
	}
	return pType_->name();
}


std::string EditorChunkEntity::idGet() const
{
	return guid_.toString();
}


bool EditorChunkEntity::clientOnlySet( const bool & b )
{
	clientOnly_ = b;
	return true;
}


void EditorChunkEntity::calculateModel()
{
	SimpleMutexHolder loadingHolder(s_loadingModelMutex_);

	// If the model is loading in the background but has not completed then
	// this is a request to change the model to something else.  The best we
	// can do is wait for the model to finish loading and then reissue another
	// background loading request.
	waitDoneLoading();

	modelToLoad_ = s_defaultModel;

	if (pyClass_)
	{
		// The tuple will dec this; we don't want it to
		Py_XINCREF( pDict_ );

		PyObject * args = PyTuple_New( 1 );
		PyTuple_SetItem( args, 0, pDict_ );

		PyObject * result = Script::ask(
			PyObject_GetAttrString( pyClass_, "modelName" ),
			args,
			"EditorChunkEntity::calculateModel: ",
			true /* ok if function NULL */);

		if (result)
		{
			if ( PyString_Check( result ) )
				modelToLoad_ = PyString_AsString( result );

			if ( modelToLoad_.empty() )
				modelToLoad_ = s_defaultModel;

			if ( modelToLoad_.length() < 7  ||
					modelToLoad_.substr( modelToLoad_.length() - 6 ) != ".model" )
				modelToLoad_ += ".model";
		}
	}

	incRef(); // Don't delete until the loading is done!

	loadBackgroundTask_ =
		new CStyleBackgroundTask
		(
			loadModelTask    , this,
			loadModelTaskDone, this
		);
	BgTaskManager::instance().addBackgroundTask( loadBackgroundTask_ );
}

/**
 *	Get the representative model for this entity
 */
ModelPtr EditorChunkEntity::reprModel() const
{
	return model_;
}

bool EditorChunkEntity::isDefaultModel() const
{
	if (model_)
        return model_->resourceID() == s_defaultModel;

	// else assume it is!
	return true;
}


void EditorChunkEntity::guid( UniqueID newGUID )
{
	guid_ = newGUID;
	chunkItemLinker()->guid(guid_);
}


void EditorChunkEntity::markModelDirty()
{
	SimpleMutexHolder permission( s_dirtyModelMutex_ );

	if (std::find(s_dirtyModelEntities_.begin(), s_dirtyModelEntities_.end(), this) == s_dirtyModelEntities_.end())
		s_dirtyModelEntities_.push_back(this);
}

void EditorChunkEntity::removeFromDirtyList()
{
	SimpleMutexHolder permission( s_dirtyModelMutex_ );

	std::vector<EditorChunkEntity*>::iterator it =
		std::find(s_dirtyModelEntities_.begin(), s_dirtyModelEntities_.end(), this);

	if (it != s_dirtyModelEntities_.end())
		s_dirtyModelEntities_.erase( it );
}

void EditorChunkEntity::calculateDirtyModels()
{
	SimpleMutexHolder permission( s_dirtyModelMutex_ );

	uint i = 0;
	while (i < EditorChunkEntity::s_dirtyModelEntities_.size())
	{
		EditorChunkEntity* ent = EditorChunkEntity::s_dirtyModelEntities_[i];

		if (!ent->chunk() || !ent->chunk()->online())
		{
			++i;
		}
		else
		{
			ent->calculateModel();
			EditorChunkEntity::s_dirtyModelEntities_.erase(EditorChunkEntity::s_dirtyModelEntities_.begin() + i);
		}
	}
}

Vector3 EditorChunkEntity::edMovementDeltaSnaps()
{
	if (pType_ && pType_->name() == std::string( "Door" ))
	{
		return Vector3( 1.f, 1.f, 1.f );
	}

	return EditorChunkItem::edMovementDeltaSnaps();
}

float EditorChunkEntity::edAngleSnaps()
{
	if (pType_ && pType_->name() == std::string( "Door" ))
	{
		return 90.f;
	}

	return EditorChunkItem::edAngleSnaps();
}


/*static*/ void EditorChunkEntity::loadModelTask(void *self)
{
	EditorChunkEntity *entity = static_cast<EditorChunkEntity *>(self);
	try
	{
		entity->loadingModel_ = Model::get(entity->modelToLoad_);
		if (!entity->loadingModel_)
		{
			ERROR_MSG( "EditorChunkEntity::calculateModel - fail to find model %s\n"
						"Substituting with default model\n",
						entity->modelToLoad_.c_str() );

			entity->loadingModel_ = Model::get( s_defaultModel );
		}
	}
	catch (...)
	{
		ERROR_MSG("EditorChunkEntity::loadModelTask crash in load\n");
	}
}


/*static*/ void EditorChunkEntity::loadModelTaskDone(void *self)
{
	EditorChunkEntity *entity = static_cast<EditorChunkEntity *>(self);

	// the variable 'oldModelHolder' will keep the model alive until the method
	// returns, so the ChunkModelObstacle doesn't crash with an BB reference to
	// a deleted object (we should really change that bb referencing)
	ModelPtr oldModelHolder = entity->model_;

	entity->model_ = entity->loadingModel_;
	if (!entity->model_)
	{
		std::string errMsg = L("Unable to load entity's model %0", entity->modelToLoad_);
		ERROR_MSG(errMsg.c_str());
	}

	// Update the collision scene
	if (!entity->surrogateMarker_)
	{
		Chunk* c = entity->chunk();
		if (c != NULL)
		{
			// don't let the ref count go to 0 in the following commands
			ChunkItemPtr ourself = entity;

			c->delStaticItem( entity );
			c->addStaticItem( entity );
			entity->syncInit();
		}
	}

	// Cleanup some unused memory:
	entity->loadingModel_ = NULL;
	entity->modelToLoad_.clear();
	entity->loadBackgroundTask_ = NULL;
	entity->decRef(); // Entity can be deleted now if necessary
}


bool EditorChunkEntity::loading() const
{
	return loadBackgroundTask_ != NULL;
}


void EditorChunkEntity::waitDoneLoading()
{
	while (loading())
	{
		::Sleep(20);
		BgTaskManager::instance().tick();
	}
}


void EditorChunkEntity::syncInit()
{
	#if UMBRA_ENABLE
	/* We need to clear the model here
	   because for entities and UDO's, the model can change
    */
	pUmbraModel_ = NULL;
	pUmbraObject_ = NULL;
	if (!this->reprModel())
	{	
		return;
	}
	BoundingBox bb = BoundingBox::s_insideOut_;
	// Grab the visibility bounding box
	bb = this->reprModel()->visibilityBox();	
	pUmbraModel_ = UmbraModelProxy::getObbModel( bb.minBounds(), bb.maxBounds() );
	pUmbraObject_ = UmbraObjectProxy::get( pUmbraModel_ );
	
	// Set the user pointer up to point at this chunk item
	pUmbraObject_->object()->setUserPointer( (void*)this );

	// Set up object transforms
	Matrix m = pChunk_->transform();
	m.preMultiply( transform_ );
	pUmbraObject_->object()->setObjectToCellMatrix( (Umbra::Matrix4x4&)m );
	pUmbraObject_->object()->setCell( pChunk_->getUmbraCell() );
	#endif
}


/// Write the factory statics stuff
#undef IMPLEMENT_CHUNK_ITEM_ARGS
#define IMPLEMENT_CHUNK_ITEM_ARGS (pSection, pChunk, &errorString)
IMPLEMENT_CHUNK_ITEM( EditorChunkEntity, entity, 1 )


// editor_chunk_entity.cpp
