/*
    This file is a part of SORT(Simple Open Ray Tracing), an open-source cross
    platform physically based renderer.
 
    Copyright (c) 2011-2018 by Cao Jiayin - All rights reserved.
 
    SORT is a free software written for educational purpose. Anyone can distribute
    or modify it under the the terms of the GNU General Public License Version 3 as
    published by the Free Software Foundation. However, there is NO warranty that
    all components are functional in a perfect manner. Without even the implied
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.
 
    You should have received a copy of the GNU General Public License along with
    this program. If not, see <http://www.gnu.org/licenses/gpl-3.0.html>.
 */

#include <memory>
#include "scene.h"
#include "math/intersection.h"
#include "accel/accelerator.h"
#include "core/path.h"
#include "core/samplemethod.h"
#include "core/sassert.h"
#include "managers/matmanager.h"
#include "light/light.h"
#include "shape/shape.h"
#include "core/sassert.h"
#include "core/stats.h"
#include "entity/visual_entity.h"
#include "entity/visual.h"
#include "core/primitive.h"
#include "stream/stream.h"
#include "stream/fstream.h"
#include "core/classtype.h"
#include "core/globalconfig.h"

SORT_STATS_DEFINE_COUNTER(sScenePrimitiveCount)
SORT_STATS_DEFINE_COUNTER(sSceneLightCount)

SORT_STATS_COUNTER("Statistics", "Total Primitive Count", sScenePrimitiveCount);
SORT_STATS_COUNTER("Statistics", "Total Light Count", sSceneLightCount);

// load the scene from script file
bool Scene::LoadScene( TiXmlNode* root )
{
	// parse the triangle mesh
	TiXmlElement* meshNode = root->FirstChildElement( "Entity" );
	while( meshNode )
	{
		// Get the name of the file
		const char* filename = meshNode->Attribute( "filename" );

		if( filename != nullptr ){
			IFileStream s(GetFullPath(filename));
			IStreamBase& stream = s;

			// Instance the entity
			unsigned int class_id = 0;
			stream >> class_id;
			std::shared_ptr<Entity>	entity = MakeEntity( class_id );

			// Serialize the entity
            if (entity) {
                entity->Serialize(stream);

                m_entities.push_back(entity);
            }
		}

		// get to the next model
		meshNode = meshNode->NextSiblingElement( "Entity" );
	}
	// generate triangle buffer after parsing from file
	_generatePriBuf();
	_genLightDistribution();
	
    SORT_STATS(sScenePrimitiveCount=(StatsInt)m_primitiveBuf.size());
    SORT_STATS(sSceneLightCount=(StatsInt)m_lights.size());

	return true;
}

// get the intersection between a ray and the scene
bool Scene::GetIntersect( const Ray& r , Intersection* intersect ) const
{
	if( intersect )
		intersect->t = FLT_MAX;

	// brute force intersection test if there is no accelerator
	if( GlobalConfiguration::GetSingleton().GetAccelerator() == nullptr )
		return _bfIntersect( r , intersect );

	return GlobalConfiguration::GetSingleton().GetAccelerator()->GetIntersect( r , intersect );
}

// get the intersection between a ray and the scene in a brute force way
bool Scene::_bfIntersect( const Ray& r , Intersection* intersect ) const
{
	if( intersect ) intersect->t = FLT_MAX;
	int n = (int)m_primitiveBuf.size();
	for( int k = 0 ; k < n ; k++ )
	{
		bool flag = m_primitiveBuf[k]->GetIntersect( r , intersect );
		if( flag && intersect == 0 )
			return true;
	}

	if( intersect == 0 )
		return false;
	return intersect->t < r.m_fMax && ( intersect->primitive != 0 );
}

// release the memory of the scene
void Scene::Release()
{
	std::vector<Primitive*>::iterator it = m_primitiveBuf.begin();
	while( it != m_primitiveBuf.end() )
		delete *it++;
	m_primitiveBuf.clear();
}

// generate primitive buffer
void Scene::_generatePriBuf()
{
	std::vector<std::shared_ptr<Entity>>::const_iterator it = m_entities.begin();
	while( it != m_entities.end() ){
		(*it)->FillScene( *this );
		it++;
	}
}

// preprocess
void Scene::PreProcess()
{
	// set uniform grid as acceleration structure as default
	if( std::shared_ptr<Accelerator> accel = GlobalConfiguration::GetSingleton().GetAccelerator() )
	{
        accel->SetPrimitives( &m_primitiveBuf );
        accel->Build();
	}
}

// parse transformation
Transform Scene::_parseTransform( const TiXmlElement* node )
{
	Transform transform;
	if( node )
	{
		node = node->FirstChildElement( "Matrix" );
		while( node )
		{
			const char* trans = node->Attribute( "value" );
			if( trans )	transform = TransformFromStr( trans ) * transform;
			node = node->NextSiblingElement( "Matrix" );
		}
	}
	return transform;
}

// get the bounding box for the scene
const BBox& Scene::GetBBox() const
{
	if( GlobalConfiguration::GetSingleton().GetAccelerator() != nullptr )
		return GlobalConfiguration::GetSingleton().GetAccelerator()->GetBBox();

	// if there is no bounding box for the scene, generate one
	std::vector<Primitive*>::const_iterator it = m_primitiveBuf.begin();
	while( it != m_primitiveBuf.end() )
	{
		m_BBox.Union( (*it)->GetBBox() );
		it++;
	}
	return m_BBox;
}

// compute light cdf
void Scene::_genLightDistribution()
{
	unsigned count = (unsigned)m_lights.size();
	if( count == 0 )
		return ;

	float* pdf = new float[count];
	for( unsigned i = 0 ; i < count ; i++ )
		pdf[i] = m_lights[i]->Power().GetIntensity();

	float total_pdf = 0.0f;
	for( unsigned i = 0 ; i < count ; i++ )
		total_pdf += pdf[i];

	for( unsigned i = 0 ; i < count ; i++ )
		m_lights[i]->SetPickPDF( pdf[i] / total_pdf );

    m_lightsDis = std::make_shared<Distribution1D>(pdf, count);
	delete[] pdf;
}

// get sampled light
const std::shared_ptr<Light> Scene::SampleLight( float u , float* pdf ) const
{
	sAssert( u >= 0.0f && u <= 1.0f , SAMPLING );
	sAssertMsg(m_lightsDis != nullptr , SAMPLING , "No light in the scene." );

	float _pdf;
	int id = m_lightsDis->SampleDiscrete( u , &_pdf );
	if( id >= 0 && id < (int)m_lights.size() && _pdf != 0.0f ){
		if( pdf ) *pdf = _pdf;
		return m_lights[id];
	}
	return nullptr;
}

// get light sample property
float Scene::LightProperbility( unsigned i ) const
{
	sAssert(m_lightsDis != nullptr , LIGHT );
	return m_lightsDis->GetProperty( i );
}

// Evaluate sky
Spectrum Scene::Le( const Ray& ray ) const
{
	if( m_skyLight )
	{
		Spectrum r;
		m_skyLight->Le( ray , 0 , r );
		return r;
	}
	return 0.0f;
}