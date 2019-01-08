#include "le_backend_vk/le_backend_vk.h"
#include "le_backend_vk/le_backend_types_internal.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <set>
#include <unordered_map>

#include "experimental/filesystem" // for parsing shader source file paths
#include <fstream>                 // for reading shader source files
#include <cstring>                 // for memcpy

#include "le_shader_compiler/le_shader_compiler.h"
#include "util/spirv-cross/spirv_cross.hpp"
#include "pal_file_watcher/pal_file_watcher.h"  // for watching shader source files
#include "le_backend_vk/util/spooky/SpookyV2.h" // for hashing renderpass gestalt, so that we can test for *compatible* renderpasses

namespace std {
using namespace experimental; // so that we can use std::filesystem as such
}

struct le_shader_module_o {
	uint64_t                                         hash                = 0;     ///< hash taken from spirv code + filepath hash
	uint64_t                                         hash_file_path      = 0;     ///< hash taken from filepath (canonical)
	uint64_t                                         hash_pipelinelayout = 0;     ///< hash taken from descriptors over all sets
	std::vector<le_shader_binding_info>              bindings;                    ///< info for each binding, sorted asc.
	std::vector<uint32_t>                            spirv    = {};               ///< spirv source code for this module
	std::filesystem::path                            filepath = {};               ///< path to source file
	std::vector<std::string>                         vertexAttributeNames;        ///< (used for debug only) name for vertex attribute
	std::vector<vk::VertexInputAttributeDescription> vertexAttributeDescriptions; ///< descriptions gathered from reflection if shader type is vertex
	std::vector<vk::VertexInputBindingDescription>   vertexBindingDescriptions;   ///< descriptions gathered from reflection if shader type is vertex
	VkShaderModule                                   module = nullptr;
	le::ShaderStage                                  stage  = {};
};

struct le_shader_manager_o {
	vk::Device device = nullptr;

	std::vector<le_shader_module_o *>                               shaderModules;         // OWNING. Stores all shader modules used in backend.
	std::unordered_map<std::string, std::set<le_shader_module_o *>> moduleDependencies;    // map 'canonical shader source file path' -> [shader modules]
	std::set<le_shader_module_o *>                                  modifiedShaderModules; // non-owning pointers to shader modules which need recompiling (used by file watcher)

	le_shader_compiler_o *shader_compiler   = nullptr; // owning
	pal_file_watcher_o *  shaderFileWatcher = nullptr; // owning
};

struct le_pipeline_manager_o {
	// TODO: These resources are potentially in-flight, and may be used read-only
	// by more than one frame - but they can only be written to one thread at a time.

	vk::Device device = nullptr;

	vk::PipelineCache vulkanCache = nullptr;

	le_shader_manager_o *shaderManager = nullptr; // owning

	std::vector<graphics_pipeline_state_o *> graphicsPSO_list; // indexed by graphicsPSO_handles
	std::vector<le_gpso_handle>              graphicsPSO_handles;

	std::vector<compute_pipeline_state_o *> computePSO_list; // indexed by computePSO_handles
	std::vector<le_cpso_handle>             computePSO_handles;

	std::unordered_map<uint64_t, vk::Pipeline, IdentityHash>            pipelines;
	std::unordered_map<uint64_t, le_pipeline_layout_info, IdentityHash> pipelineLayoutInfos;

	std::unordered_map<uint64_t, le_descriptor_set_layout_t, IdentityHash> descriptorSetLayouts; // indexed by le_shader_bindings_info[] hash
	std::unordered_map<uint64_t, vk::PipelineLayout, IdentityHash>         pipelineLayouts;      // indexed by hash of array of descriptorSetLayoutCache keys per pipeline layout
};

// ----------------------------------------------------------------------
/// \brief   file loader utility method
/// \details loads file given by filepath and returns a vector of chars if successful
/// \note    returns an empty vector if not successful
static std::vector<char> load_file( const std::filesystem::path &file_path, bool *success ) {

	std::vector<char> contents;

	size_t        fileSize = 0;
	std::ifstream file( file_path, std::ios::in | std::ios::binary | std::ios::ate );

	if ( !file.is_open() ) {
		std::cerr << "Unable to open file: " << std::filesystem::canonical( file_path ) << std::endl
		          << std::flush;
		*success = false;
		return contents;
	}

	//	std::cout << "OK Opened file:" << std::filesystem::canonical( file_path ) << std::endl
	//	          << std::flush;

	// ----------| invariant: file is open

	auto endOfFilePos = file.tellg();

	if ( endOfFilePos > 0 ) {
		fileSize = size_t( endOfFilePos );
	} else {
		*success = false;
		return contents;
	}

	// ----------| invariant: file has some bytes to read
	contents.resize( fileSize );

	file.seekg( 0, std::ios::beg );
	file.read( contents.data(), endOfFilePos );
	file.close();

	*success = true;
	return contents;
}

// ----------------------------------------------------------------------
// Returns the hash for a given shaderModule
static uint64_t le_shader_module_get_hash( le_shader_module_o *module ) {
	assert( module != nullptr );
	return module->hash;
}

// Returns the stage for a given shader module
static LeShaderStageEnum le_shader_module_get_stage( le_shader_module_o *module ) {
	assert( module != nullptr );
	return {module->stage};
}

// ----------------------------------------------------------------------

static bool check_is_data_spirv( const void *raw_data, size_t data_size ) {

	struct SpirVHeader {
		uint32_t magic; // Spirv magic number
		union {         // Spir-V version number, bytes (high to low): [0x00, major, minor, 0x00]
			struct {
				uint8_t padding_low;
				uint8_t version_minor;
				uint8_t version_major;
				uint8_t padding_hi;
			};
			uint32_t version_number;
		};
		uint32_t gen_magic; // Generator magic number
		uint32_t bound;     //
		uint32_t reserved;  // Reserved
	} file_header;

	if ( data_size < sizeof( SpirVHeader ) ) {
		// Ahem, file does not even contain a header, what were you thinking?
		return false;
	}

	// ----------| invariant: file contains enough bytes for a valid file header

	static const uint32_t SPIRV_MAGIC = 0x07230203; // magic number for spir-v files, see: <https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html#_a_id_physicallayout_a_physical_layout_of_a_spir_v_module_and_instruction>

	memcpy( &file_header, raw_data, sizeof( file_header ) );

	if ( file_header.magic == SPIRV_MAGIC ) {
		return true;
	} else {
		// Invalid file header for spir-v file.
		//		std::cerr << "ERROR: Invalid header for SPIR-V file detected." << std::endl
		//		          << std::flush;
		return false;
	}
}

// ----------------------------------------------------------------------

/// \brief translate a binary blob into spirv code if possible
/// \details Blob may be raw spirv data, or glsl data
static void le_pipeline_cache_translate_to_spirv_code( le_shader_compiler_o *shader_compiler, void *raw_data, size_t numBytes, LeShaderStageEnum moduleType, const char *original_file_name, std::vector<uint32_t> &spirvCode, std::set<std::string> &includesSet ) {

	if ( check_is_data_spirv( raw_data, numBytes ) ) {
		spirvCode.resize( numBytes / 4 );
		memcpy( spirvCode.data(), raw_data, numBytes );
	} else {
		// Data is not spirv - is it glsl, perhaps?

		using namespace le_shader_compiler; // for compiler_i

		auto compileResult = compiler_i.compile_source( shader_compiler, static_cast<const char *>( raw_data ), numBytes, moduleType, original_file_name );

		if ( compiler_i.get_result_success( compileResult ) == true ) {
			const char *addr;
			size_t      res_sz;
			compiler_i.get_result_bytes( compileResult, &addr, &res_sz );
			spirvCode.resize( res_sz / 4 );
			memcpy( spirvCode.data(), addr, res_sz );

			// -- grab a list of includes which this compilation unit depends on:

			const char *pStr;
			size_t      strSz = 0;

			while ( compiler_i.get_result_includes( compileResult, &pStr, &strSz ) ) {
				// -- update set of includes for this module
				includesSet.emplace( pStr, strSz );
			}
		}

		// release compile result object
		compiler_i.release_result( compileResult );
	}
}

// ----------------------------------------------------------------------

// Flags all modules which are affected by a change in shader_source_file_path,
// and adds them to a set of shader modules wich need to be recompiled.
// Note: This method is called via a file changed callback.
static void le_pipeline_cache_flag_affected_modules_for_source_path( le_shader_manager_o *self, const char *shader_source_file_path ) {
	// find all modules from dependencies set
	// insert into list of modified modules.

	if ( 0 == self->moduleDependencies.count( shader_source_file_path ) ) {
		// -- no matching dependencies.
		std::cout << "Shader code update detected, but no modules using shader source file: " << shader_source_file_path << std::endl
		          << std::flush;
		return;
	}

	// ---------| invariant: at least one module depends on this shader source file.

	auto const &moduleDependencies = self->moduleDependencies[ shader_source_file_path ];

	// -- add all affected modules to the set of modules which depend on this shader source file.

	for ( auto const &m : moduleDependencies ) {
		self->modifiedShaderModules.insert( m );
	}
};

// ----------------------------------------------------------------------

static void le_pipeline_cache_set_module_dependencies_for_watched_file( le_shader_manager_o *self, le_shader_module_o *module, std::set<std::string> &sourcePaths ) {

	// To be able to tell quick which modules need to be recompiled if a source file changes,
	// we build a table from source file -> array of modules

	for ( const auto &s : sourcePaths ) {

		// If no previous entry for this source path existed, we must insert a watch for this path
		// the watch will call a backend method which figures out how many modules were affected.
		if ( 0 == self->moduleDependencies.count( s ) ) {

			// this is the first time this file appears on our radar. Let's create a file watcher for it.
			static auto &file_watcher_i = *Registry::getApi<pal_file_watcher_i>();

			pal_file_watcher_watch_settings settings;
			settings.filePath           = s.c_str();
			settings.callback_user_data = self;
			settings.callback_fun       = []( const char *path, void *user_data ) -> bool {
                auto shader_manager = static_cast<le_shader_manager_o *>( user_data );
                // call a method on backend to tell it that the file path has changed.
                // backend to figure out which modules are affected.
                le_pipeline_cache_flag_affected_modules_for_source_path( shader_manager, path );
                return true;
			};
			file_watcher_i.add_watch( self->shaderFileWatcher, settings );
		}

		std::cout << std::hex << module << " : " << s << std::endl
		          << std::flush;

		self->moduleDependencies[ s ].insert( module );
	}
}

// ----------------------------------------------------------------------
// Calculates a hash of hashes over all pipeline layout hashes over all
// shader stages held in array shader_modules
// Note that shader_modules must have not more than 16 elements.
static uint64_t shader_modules_get_pipeline_layout_hash( le_shader_module_o const *const *shader_modules, size_t numModules ) {

	assert( numModules <= 16 ); // note max 16 shader modules.

	// We use a stack-allocated c-array instead of vector so that
	// temporary allocations happens on the stack and not on the
	// free store. The number of shader modules will always be very
	// small.

	uint64_t pipeline_layout_hash_data[ 16 ] = {};

	le_shader_module_o const *const *end_shader_modules = shader_modules + numModules;

	uint64_t *elem = pipeline_layout_hash_data; // Get first element

	for ( auto s = shader_modules; s != end_shader_modules; s++ ) {
		*elem = ( *s )->hash_pipelinelayout;
		elem++;
	}

	return SpookyHash::Hash64( pipeline_layout_hash_data, numModules * sizeof( uint64_t ), 0 );
}

// ----------------------------------------------------------------------
/// \returns stride (in Bytes) for a given spirv type object
static uint32_t spirv_type_get_stride( const spirv_cross::SPIRType &spir_type ) {
	// NOTE: spir_type.width is given in bits
	return ( spir_type.width / 8 ) * spir_type.vecsize * spir_type.columns;
}

// clang-format off
// ----------------------------------------------------------------------
/// \returns corresponding vk::Format for a given spirv type object
static vk::Format spirv_type_get_vk_format( const spirv_cross::SPIRType &spirv_type ) {

	if ( spirv_type.columns != 1 ){
		assert(false); // columns must be 1 for a vkFormat
		return vk::Format::eUndefined;
	}

	// ----------| invariant: columns == 1

	switch ( spirv_type.basetype ) {
	case spirv_cross::SPIRType::Float:
		switch ( spirv_type.vecsize ) {
		case 4: return vk::Format::eR32G32B32A32Sfloat;
		case 3: return vk::Format::eR32G32B32Sfloat;
		case 2: return vk::Format::eR32G32Sfloat;
		case 1: return vk::Format::eR32Sfloat;
		}
	    break;
	case spirv_cross::SPIRType::Half:
		switch ( spirv_type.vecsize ) {
		case 4: return vk::Format::eR16G16B16A16Sfloat;
		case 3: return vk::Format::eR16G16B16Sfloat;
		case 2: return vk::Format::eR16G16Sfloat;
		case 1: return vk::Format::eR16Sfloat;
		}
	    break;
	case spirv_cross::SPIRType::Int:
		switch ( spirv_type.vecsize ) {
		case 4: return vk::Format::eR32G32B32A32Sint;
		case 3: return vk::Format::eR32G32B32Sint;
		case 2: return vk::Format::eR32G32Sint;
		case 1: return vk::Format::eR32Sint;
		}
	    break;
	case spirv_cross::SPIRType::UInt:
		switch ( spirv_type.vecsize ) {
		case 4: return vk::Format::eR32G32B32A32Uint;
		case 3: return vk::Format::eR32G32B32Uint;
		case 2: return vk::Format::eR32G32Uint;
		case 1: return vk::Format::eR32Uint;
		}
	    break;
	case spirv_cross::SPIRType::Char:
		switch ( spirv_type.vecsize ) {
		case 4: return vk::Format::eR8G8B8A8Unorm;
		case 3: return vk::Format::eR8G8B8Unorm;
		case 2: return vk::Format::eR8G8Unorm;
		case 1: return vk::Format::eR8Unorm;
		}
	    break;
	default:
		assert(false); // format not covered by switch case.
	break;
	}

	assert(false); // something went wrong.
	return vk::Format::eUndefined;
}
// clang-format on
// ----------------------------------------------------------------------

static void shader_module_update_reflection( le_shader_module_o *module ) {

	std::vector<le_shader_binding_info>              bindings;                    // <- gets stored in module at end
	std::vector<vk::VertexInputAttributeDescription> vertexAttributeDescriptions; // <- gets stored in module at end
	std::vector<vk::VertexInputBindingDescription>   vertexBindingDescriptions;   // <- gets stored in module at end
	std::vector<std::string>                         vertexAttributeNames;        // <- gets stored in module at end

	static_assert( sizeof( le_shader_binding_info ) == sizeof( uint64_t ) * 2, "Shader binding info must be the same size as 2 * uint64_t" );

	spirv_cross::Compiler compiler( module->spirv );

	// The SPIR-V is now parsed, and we can perform reflection on it.
	spirv_cross::ShaderResources resources = compiler.get_shader_resources();

	{ // -- find out max number of bindings
		size_t bindingsCount = resources.uniform_buffers.size() +
		                       resources.storage_buffers.size() +
		                       resources.storage_images.size() +
		                       resources.sampled_images.size();

		bindings.reserve( bindingsCount );
	}

	// If this shader module represents a vertex shader, get
	// stage_inputs, as these represent vertex shader inputs.
	if ( module->stage == le::ShaderStage::eVertex ) {

		uint32_t location = 0; // shader location qualifier mapped to binding number

		// NOTE:  resources.stage_inputs means inputs to this shader stage
		//		  resources.stage_outputs means outputs from this shader stage.
		vertexAttributeDescriptions.reserve( resources.stage_inputs.size() );
		vertexBindingDescriptions.reserve( resources.stage_inputs.size() );
		vertexAttributeNames.reserve( resources.stage_inputs.size() );

		// NOTE: we assume that stage_inputs are ordered ASC by location
		for ( auto const &stageInput : resources.stage_inputs ) {

			if ( compiler.get_decoration_bitset( stageInput.id ).get( spv::DecorationLocation ) ) {
				location = compiler.get_decoration( stageInput.id, spv::DecorationLocation );
			}

			auto const &attributeType = compiler.get_type( stageInput.type_id );

			// We create one binding description for each attribute description,
			// which means that vertex input is assumed to be not interleaved.
			//
			// User may override reflection-generated vertex input by explicitly
			// specifying vertex input when creating pipeline.

			vk::VertexInputAttributeDescription inputAttributeDescription{};
			vk::VertexInputBindingDescription   vertexBindingDescription{};

			inputAttributeDescription
			    .setLocation( location )                                // by default, we assume one buffer per location
			    .setBinding( location )                                 // by default, we assume one buffer per location
			    .setFormat( spirv_type_get_vk_format( attributeType ) ) // best guess, derived from spirv_type
			    .setOffset( 0 );                                        // non-interleaved means offset must be 0

			vertexBindingDescription
			    .setBinding( location )
			    .setInputRate( vk::VertexInputRate::eVertex )
			    .setStride( spirv_type_get_stride( attributeType ) );

			vertexAttributeDescriptions.emplace_back( std::move( inputAttributeDescription ) );
			vertexBindingDescriptions.emplace_back( std::move( vertexBindingDescription ) );
			vertexAttributeNames.emplace_back( stageInput.name );

			++location;
		}

		// store vertex input info with module

		module->vertexAttributeDescriptions = std::move( vertexAttributeDescriptions );
		module->vertexBindingDescriptions   = std::move( vertexBindingDescriptions );
		module->vertexAttributeNames        = std::move( vertexAttributeNames );
	}

	// -- Get all sampled images in the shader
	for ( auto const &resource : resources.sampled_images ) {
		// TODO: how do we deal with arrays of images?
		// it is well possible that spirv cross reports each image individually, giving it an index.
		le_shader_binding_info info{};

		info.setIndex   = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
		info.binding    = compiler.get_decoration( resource.id, spv::DecorationBinding );
		info.type       = enumToNum( vk::DescriptorType::eCombinedImageSampler ); // Note: sampled_images corresponds to combinedImageSampler, separate_[image|sampler] corresponds to image, and sampler being separate
		info.stage_bits = enumToNum( module->stage );
		info.count      = 1;
		info.name_hash  = hash_64_fnv1a( resource.name.c_str() );

		bindings.emplace_back( std::move( info ) );
	}

	// -- Get all uniform buffers in shader
	for ( auto const &resource : resources.uniform_buffers ) {
		le_shader_binding_info info{};

		info.setIndex   = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
		info.binding    = compiler.get_decoration( resource.id, spv::DecorationBinding );
		info.type       = enumToNum( vk::DescriptorType::eUniformBufferDynamic );
		info.count      = 1;
		info.stage_bits = enumToNum( module->stage );
		info.name_hash  = hash_64_fnv1a( resource.name.c_str() );
		info.range      = compiler.get_declared_struct_size( compiler.get_type( resource.type_id ) );

		bindings.emplace_back( std::move( info ) );
	}

	// -- Get all storage buffers in shader
	for ( auto &resource : resources.storage_buffers ) {
		le_shader_binding_info info{};

		info.setIndex   = compiler.get_decoration( resource.id, spv::DecorationDescriptorSet );
		info.binding    = compiler.get_decoration( resource.id, spv::DecorationBinding );
		info.type       = enumToNum( vk::DescriptorType::eStorageBufferDynamic );
		info.count      = 1;
		info.stage_bits = enumToNum( module->stage );
		info.name_hash  = hash_64_fnv1a( resource.name.c_str() );

		bindings.emplace_back( std::move( info ) );
	}

	// Sort bindings - this makes it easier for us to link shader stages together
	std::sort( bindings.begin(), bindings.end() ); // we're sorting shader bindings by set, binding ASC

	// -- calculate hash over bindings
	module->hash_pipelinelayout = SpookyHash::Hash64( bindings.data(), sizeof( le_shader_binding_info ) * bindings.size(), 0 );

	// -- store bindings with module
	module->bindings = std::move( bindings );
}

// ----------------------------------------------------------------------

static bool shader_module_check_bindings_valid( le_shader_binding_info const *bindings, size_t numBindings ) {
	// -- perform sanity check on bindings - bindings must be unique (location+binding cannot be shared between shader uniforms)
	auto b_start = bindings;
	auto b_end   = b_start + numBindings;

	// compare sorted bindings and raise the alarm if two successive bindings alias locations

	for ( auto b = b_start, b_prev = b_start; b != b_end; b++ ) {

		if ( b == b_prev ) {
			// first iteration
			continue;
		}

		if ( b->setIndex == b_prev->setIndex &&
		     b->binding == b_prev->binding ) {
			std::cerr << "ERROR: Illegal shader bindings detected, rejecting shader.\n\tDuplicate bindings for set: " << b->setIndex << ", binding: " << b->binding;
			return false;
		}

		b_prev = b;
	}

	return true;
}

// ----------------------------------------------------------------------

// Create union of bindings over shader stages based on the
// invariant that each shader stage provides their bindings
// in ascending order.
//
// Returns a vector with binding info, combined over all shader stages given.
// Note: Bindings must not be sparse, otherwise this method will assert(false)
//
static std::vector<le_shader_binding_info> shader_modules_get_bindings_list( le_shader_module_o const *const *shaderStages, size_t numStages ) {

	std::vector<le_shader_binding_info> combined_bindings;

	size_t maxNumBindings = 0;

	// maxNumBindings holds the upper bound for the total number of bindings
	// assuming no overlaps in bindings between shader stages.

	auto end_shaderStages = shaderStages + numStages;

	for ( auto s = shaderStages; s != end_shaderStages; s++ ) {
		maxNumBindings += ( *s )->bindings.size();
	}

	// Reserve space for the worst casse so that we avoid internal
	// re-allocations when appending to combined_bindings.
	combined_bindings.reserve( maxNumBindings );

	// We want to iterate over all bindings over all shader stages.
	// For this, we store the base pointer of each shader stage's
	// bindings in a vector, one vector entry per shader stage.
	std::vector<le_shader_binding_info const *> pBindings;
	pBindings.reserve( numStages );

	// So that we know when to stop iterating over bindings for each shader, we store
	// the end pointer of the bindings for each shader stage, one vector entry per shader stage.
	// Vector indices match with pBindings
	std::vector<le_shader_binding_info const *> pBindingsEnd; // end marker for infos, per shader stage
	pBindingsEnd.reserve( numStages );

	for ( auto s = shaderStages; s != end_shaderStages; s++ ) {
		pBindings.push_back( ( *s )->bindings.data() );
	}

	for ( auto s = shaderStages; s != end_shaderStages; s++ ) {
		pBindingsEnd.emplace_back( ( *s )->bindings.data() + ( *s )->bindings.size() );
	}

	// Note that we could execute the following loop as an unbounded
	// while loop, but we choose to use maxNumBinings as the upper limit
	// for this loop - it is well conceivable that we return earlier
	// because a break condition is met.
	for ( size_t nb = 0; nb != maxNumBindings; ++nb ) {

		// Find the lowest binding, and push it back to the
		// vector of combined bindings

		// If a binding does only exist in one shader, we
		// add it to the list of combined bindings.

		// If a binding exists in more than one shader, we
		// must first check if it can be combined with a
		// binding in another shader at the same position

		bool noMoreBindings = true;
		for ( size_t i = 0; i != numStages; ++i ) {

			if ( pBindings[ i ] != pBindingsEnd[ i ] ) {
				noMoreBindings = false;
				break;
			}
		}

		if ( noMoreBindings ) {
			// we have processed all bindings.
			break;
		}

		// ---------- invariant: there are bindings to process

		// Now we want all bindings which refer to the same set index and binding
		// so that we can make sure they can be combined.

		uint64_t set_binding_mask = 0;
		{
			// switch on elements which we want to compare against
			le_shader_binding_info info{};
			info.binding     = ~info.binding;
			info.setIndex    = ~info.setIndex;
			set_binding_mask = info.data;
		}

		// Find the binding with the lowest set index and binding over all shaders.
		size_t stageWithLowestSetBinding = 0;
		{

			bool isFirstIteration = true;
			for ( size_t i = 0; i != numStages; ++i ) {
				if ( pBindings[ i ] == pBindingsEnd[ i ] ) {
					continue;
				} else if ( isFirstIteration ) {
					// First shader with binding information found
					stageWithLowestSetBinding = i;
					isFirstIteration          = false;
					continue;
				} else {
					// invariant :
					// + pBindings[i] is valid
					// + pBindings[shaderWithLowestSetBinding] is valid

					if ( ( ( pBindings[ i ]->data ) & set_binding_mask ) < ( ( pBindings[ stageWithLowestSetBinding ]->data ) & set_binding_mask ) ) {
						stageWithLowestSetBinding = i;
					}
				}
			}
			assert( isFirstIteration == false ); // This should not happen, indicates that no bindings are left to process, since we didn't even get a first iteration...
		}

		// Now we need to find all shaders which refer to this, the lowest (set, binding) combination.

		std::vector<size_t> filteredStageIndices; // indices for shader stages with same set, binding
		{
			const uint64_t lowestSetBinding = ( pBindings[ stageWithLowestSetBinding ]->data ) & set_binding_mask;

			for ( size_t i = stageWithLowestSetBinding; i != numStages; ++i ) {
				if ( pBindings[ i ] == pBindingsEnd[ i ] ) {
					continue;
				} else if ( ( ( pBindings[ i ]->data ) & set_binding_mask ) == lowestSetBinding ) {
					filteredStageIndices.push_back( i );
				}
			}
		}
		assert( filteredStageIndices.size() != 0 );

		// Check whether binding data is consistent over all shaders which refer
		// to the binding with the currently lowest setBinding
		uint64_t compare_mask = 0;
		{
			// switch on elements which we want to compare against
			le_shader_binding_info info{};
			info.type     = ~info.type;
			info.binding  = ~info.binding;
			info.setIndex = ~info.setIndex;
			compare_mask  = info.data;
		}

		bool bindingDataIsConsistent = true;
		bool bindingNameIsConsistent = true;

		le_shader_binding_info const &firstBinding = *pBindings[ filteredStageIndices[ 0 ] ];

		for ( size_t i = 1; i != filteredStageIndices.size(); ++i ) {
			if ( ( pBindings[ filteredStageIndices[ i ] ]->data & compare_mask ) != ( firstBinding.data & compare_mask ) ) {
				bindingDataIsConsistent = false;
			}
			if ( pBindings[ filteredStageIndices[ i ] ]->name_hash != firstBinding.name_hash ) {
				bindingNameIsConsistent = false;
			}
		}

		if ( bindingDataIsConsistent ) {

			if ( !bindingNameIsConsistent ) {
				// This is not tragic, but we need to flag up that this binding is not
				// consistently named in case this hints at a bigger issue.

				std::cout << "Warning: Inconsistent name in set: " << firstBinding.setIndex << ", for binding: " << firstBinding.binding << std::endl;

				for ( auto i : filteredStageIndices ) {
					std::cout << "\t shader stage: " << shaderStages[ i ]->filepath << std::endl;
				}

				std::cout << "Using name given in first shader stage for this binding." << std::endl
				          << std::flush;

			} // end binding name not consistent

			// Initialise our combined binding from the first binding.
			le_shader_binding_info combinedBinding = firstBinding;

			// Combine binding information over all stages, so that a descriptor generated
			// for a binding can match all shader stages which it feeds into.

			bool first_iter = true;
			for ( auto shaderI : filteredStageIndices ) {
				if ( first_iter ) {
					first_iter = false;
					continue;
				}

				if ( combinedBinding.type == enumToNum( vk::DescriptorType::eUniformBuffer ) ||
				     combinedBinding.type == enumToNum( vk::DescriptorType::eUniformBufferDynamic ) ) {
					// If we're dealing with a buffer type, we must check ranges
					// TODO: if one of them has range == 0, that means this shader stage can be ignored
					// If they have not the same range, that means we need to take the largest range of them both
					combinedBinding.range = std::max( combinedBinding.range, pBindings[ shaderI ]->range );
				}

				// -- combine stage bits so that descriptor will be available for all stages that request it.
				combinedBinding.stage_bits = combinedBinding.stage_bits | pBindings[ shaderI ]->stage_bits;

				// if count is not identical, that's not that bad, we adjust to larger of the two
				combinedBinding.count = std::max( combinedBinding.count, pBindings[ shaderI ]->count );
			}

			// store combined binding.
			combined_bindings.emplace_back( combinedBinding );

			// Now increase bindig pointers for all shaders which were affected
			for ( auto stageI : filteredStageIndices ) {
				pBindings[ stageI ]++;
			}

		} else {

			std::cerr << "ERROR: Shader binding mismatch in set: " << firstBinding.setIndex
			          << ", binding: " << firstBinding.binding << std::endl;
			for ( auto stageI : filteredStageIndices ) {
				std::cerr << "\t shader stage : " << shaderStages[ stageI ]->filepath << std::endl;
			}

			std::cerr << std::flush;
			assert( false ); // abandon all hope.

		} // end if binding data is consistent

	} // end loop over upper bound on number of bindings over all shader stages

	return combined_bindings;
}

// ----------------------------------------------------------------------

static void le_shader_manager_shader_module_update( le_shader_manager_o *self, le_shader_module_o *module ) {

	// Shader module needs updating if shader code has changed.
	// if this happens, a new vulkan object for the module must be crated.

	// The module must be locked for this, as we need exclusive access just in case the module is
	// in use by the frame recording thread, which may want to create pipelines.
	//
	// Vulkan Lifetimes require us only to keep module alive for as long as a pipeline is being
	// generated from it. This means we "only" need to protect against any threads which might be
	// creating pipelines.

	// -- get module spirv code
	bool loadSuccessful = false;
	auto source_text    = load_file( module->filepath, &loadSuccessful );

	if ( !loadSuccessful ) {
		// file could not be loaded. bail out.
		return;
	}

	std::vector<uint32_t> spirv_code;
	std::set<std::string> includesSet;

	le_pipeline_cache_translate_to_spirv_code( self->shader_compiler, source_text.data(), source_text.size(), {module->stage}, module->filepath.c_str(), spirv_code, includesSet );

	if ( spirv_code.empty() ) {
		// no spirv code available, bail out.
		return;
	}

	// -- check spirv code hash against module spirv hash
	uint64_t hash_of_module = SpookyHash::Hash64( spirv_code.data(), spirv_code.size() * sizeof( uint32_t ), module->hash_file_path );

	if ( hash_of_module == module->hash ) {
		// spirv code identical, no update needed, bail out.
		return;
	}

	le_shader_module_o previous_module = *module; // create backup copy

	// -- update module hash
	module->hash = hash_of_module;

	// -- update additional include paths, if necessary.
	le_pipeline_cache_set_module_dependencies_for_watched_file( self, module, includesSet );

	// ---------| Invariant: new spir-v code detected.

	// -- if hash doesn't match, delete old vk module, create new vk module

	// -- store new spir-v code
	module->spirv = std::move( spirv_code );

	// -- update bindings via spirv-cross, and update bindings hash
	shader_module_update_reflection( module );

	if ( false == shader_module_check_bindings_valid( module->bindings.data(), module->bindings.size() ) ) {
		// we must clean up, and report an error
		*module = previous_module;
		return;
	}

	// -- delete old vulkan shader module object
	// Q: Should we rather defer deletion? In case that this module is in use?
	// A: Not really - according to spec module must only be alife while pipeline is being compiled.
	//    If we can guarantee that no other process is using this module at the moment to compile a
	//    Pipeline, we can safely delete it.
	self->device.destroyShaderModule( module->module );
	module->module = nullptr;

	// -- create new vulkan shader module object
	vk::ShaderModuleCreateInfo createInfo( vk::ShaderModuleCreateFlags(), module->spirv.size() * sizeof( uint32_t ), module->spirv.data() );
	module->module = self->device.createShaderModule( createInfo );
}

// ----------------------------------------------------------------------
// this method is called via renderer::update - before frame processing.
static void le_shader_manager_update_shader_modules( le_shader_manager_o *self ) {

	// -- find out which shader modules have been tainted
	static auto &file_watcher_i = *Registry::getApi<pal_file_watcher_i>();

	// this will call callbacks on any watched file objects as a side effect
	// callbacks will modify le_backend->modifiedShaderModules
	file_watcher_i.poll_notifications( self->shaderFileWatcher );

	// -- update only modules which have been tainted

	for ( auto &s : self->modifiedShaderModules ) {
		le_shader_manager_shader_module_update( self, s );
	}

	self->modifiedShaderModules.clear();
}

// ----------------------------------------------------------------------

le_shader_manager_o *le_shader_manager_create( VkDevice_T *device ) {
	auto self = new le_shader_manager_o();

	self->device = device;

	// -- create shader compiler
	using namespace le_shader_compiler;
	self->shader_compiler = compiler_i.create();

	// -- create file watcher for shader files so that changes can be detected
	static auto &file_watcher_i = *Registry::getApi<pal_file_watcher_i>();
	self->shaderFileWatcher     = file_watcher_i.create();

	return self;
}

// ----------------------------------------------------------------------

static void le_shader_manager_destroy( le_shader_manager_o *self ) {

	using namespace le_shader_compiler;

	if ( self->shaderFileWatcher ) {
		// -- destroy file watcher
		static auto &file_watcher_i = *Registry::getApi<pal_file_watcher_i>();
		file_watcher_i.destroy( self->shaderFileWatcher );
		self->shaderFileWatcher = nullptr;
	}

	if ( self->shader_compiler ) {
		// -- destroy shader compiler
		compiler_i.destroy( self->shader_compiler );
		self->shader_compiler = nullptr;
	}

	// -- destroy retained shader modules

	for ( auto &s : self->shaderModules ) {
		if ( s->module ) {
			self->device.destroyShaderModule( s->module );
			s->module = nullptr;
		}
		delete ( s );
	}
	self->shaderModules.clear();
	delete self;
}

// ----------------------------------------------------------------------
/// \brief create vulkan shader module based on file path
/// \details FIXME: this method can get called nearly anywhere - it should not be publicly accessible.
/// ideally, this method is only allowed to be called in the setup phase.
static le_shader_module_o *le_shader_manager_create_shader_module( le_shader_manager_o *self, char const *path, const LeShaderStageEnum &moduleType ) {

	// This method gets called through the renderer - it is assumed during the setup stage.

	bool loadSuccessful = false;
	auto raw_file_data  = load_file( path, &loadSuccessful ); // returns a raw byte vector

	if ( !loadSuccessful ) {
		return nullptr;
	}

	// ---------| invariant: load was successful

	// We use the canonical path to store a fingerprint of the file
	auto     canonical_path_as_string = std::filesystem::canonical( path ).string();
	uint64_t file_path_hash           = SpookyHash::Hash64( canonical_path_as_string.data(), canonical_path_as_string.size(), 0x0 );

	// -- Make sure the file contains spir-v code.

	std::vector<uint32_t> spirv_code;
	std::set<std::string> includesSet = {{canonical_path_as_string}}; // let first element be the source file path

	le_pipeline_cache_translate_to_spirv_code( self->shader_compiler, raw_file_data.data(), raw_file_data.size(), moduleType, path, spirv_code, includesSet );

	// FIXME: we need to check spirv code is ok, that compilation succeeded.

	le_shader_module_o *module = new le_shader_module_o{};

	module->stage          = moduleType;
	module->filepath       = canonical_path_as_string;
	module->hash_file_path = file_path_hash;
	module->hash           = SpookyHash::Hash64( spirv_code.data(), spirv_code.size() * sizeof( uint32_t ), module->hash_file_path );

	{
		// -- Check if module is already present in render module cache.

		auto found_module = std::find_if( self->shaderModules.begin(), self->shaderModules.end(), [module]( const le_shader_module_o *m ) -> bool {
			return module->hash == m->hash;
		} );

		// -- If module found in cache, return cached module, discard local module

		if ( found_module != self->shaderModules.end() ) {
			delete module;
			return *found_module;
		}
	}

	// ---------| invariant: no previous module with this hash exists

	module->spirv = std::move( spirv_code );

	shader_module_update_reflection( module );

	if ( false == shader_module_check_bindings_valid( module->bindings.data(), module->bindings.size() ) ) {
		// we must clean up, and report an error
		delete module;
		return nullptr;
	}

	// ----------| invariant: bindings sanity check passed

	{
		// -- create vulkan shader object
		// flags must be 0 (reserved for future use), size is given in bytes
		vk::ShaderModuleCreateInfo createInfo( vk::ShaderModuleCreateFlags(), module->spirv.size() * sizeof( uint32_t ), module->spirv.data() );

		module->module = self->device.createShaderModule( createInfo );
	}

	// -- retain module in renderer
	self->shaderModules.push_back( module );

	// -- add all source files for this file to the list of watched
	//    files that point back to this module
	le_pipeline_cache_set_module_dependencies_for_watched_file( self, module, includesSet );

	return module;
}

// ----------------------------------------------------------------------
// called via decoder / produce_frame -
static vk::PipelineLayout le_pipeline_manager_get_pipeline_layout( le_pipeline_manager_o *self, le_shader_module_o const *const *shader_modules, size_t numModules ) {

	uint64_t pipelineLayoutHash = shader_modules_get_pipeline_layout_hash( shader_modules, numModules );

	auto foundLayout = self->pipelineLayouts.find( pipelineLayoutHash );

	if ( foundLayout != self->pipelineLayouts.end() ) {
		return foundLayout->second;
	} else {
		std::cerr << "ERROR: Could not find pipeline layout with hash: " << std::hex << pipelineLayoutHash << std::endl
		          << std::flush;
		assert( false );
		return nullptr;
	}
}

// ----------------------------------------------------------------------

static inline vk::VertexInputRate vk_input_rate_from_le_input_rate( const le_vertex_input_binding_description::INPUT_RATE &input_rate ) {
	switch ( input_rate ) {
	case ( le_vertex_input_binding_description::ePerInstance ):
	    return vk::VertexInputRate::eInstance;
	case ( le_vertex_input_binding_description::ePerVertex ):
	    return vk::VertexInputRate::eVertex;
	}
	assert( false ); // something's wrong: control should never come here, switch needs to cover all cases.
	return vk::VertexInputRate::eVertex;
}

// ----------------------------------------------------------------------

// clang-format off
/// \returns corresponding vk::Format for a given le_input_attribute_description struct
static inline vk::Format vk_format_from_le_vertex_input_attribute_description( le_vertex_input_attribute_description const & d){

	if ( d.vecsize == 0 || d.vecsize > 4 ){
		assert(false); // vecsize must be between 1 and 4
		return vk::Format::eUndefined;
	}

	switch ( d.type ) {
	case le_vertex_input_attribute_description::eFloat:
		switch ( d.vecsize ) {
		case 4: return vk::Format::eR32G32B32A32Sfloat;
		case 3: return vk::Format::eR32G32B32Sfloat;
		case 2: return vk::Format::eR32G32Sfloat;
		case 1: return vk::Format::eR32Sfloat;
		}
	    break;
	case le_vertex_input_attribute_description::eHalf:
		switch ( d.vecsize ) {
		case 4: return vk::Format::eR16G16B16A16Sfloat;
		case 3: return vk::Format::eR16G16B16Sfloat;
		case 2: return vk::Format::eR16G16Sfloat;
		case 1: return vk::Format::eR16Sfloat;
		}
	    break;
	case le_vertex_input_attribute_description::eUShort: // fall through to eShort
	case le_vertex_input_attribute_description::eShort:
		if (d.isNormalised){
			switch ( d.vecsize ) {
			case 4: return vk::Format::eR16G16B16A16Unorm;
			case 3: return vk::Format::eR16G16B16Unorm;
			case 2: return vk::Format::eR16G16Unorm;
			case 1: return vk::Format::eR16Unorm;
			}
		}else{
			switch ( d.vecsize ) {
			case 4: return vk::Format::eR16G16B16A16Uint;
			case 3: return vk::Format::eR16G16B16Uint;
			case 2: return vk::Format::eR16G16Uint;
			case 1: return vk::Format::eR16Uint;
			}
		}
	    break;
	case le_vertex_input_attribute_description::eInt:
		switch ( d.vecsize ) {
		case 4: return vk::Format::eR32G32B32A32Sint;
		case 3: return vk::Format::eR32G32B32Sint;
		case 2: return vk::Format::eR32G32Sint;
		case 1: return vk::Format::eR32Sint;
		}
	    break;
	case le_vertex_input_attribute_description::eUInt:
		switch ( d.vecsize ) {
		case 4: return vk::Format::eR32G32B32A32Uint;
		case 3: return vk::Format::eR32G32B32Uint;
		case 2: return vk::Format::eR32G32Uint;
		case 1: return vk::Format::eR32Uint;
		}
	    break;
	case le_vertex_input_attribute_description::eChar:  // fall through to uChar
	case le_vertex_input_attribute_description::eUChar:
		if (d.isNormalised){
			switch ( d.vecsize ) {
			case 4: return vk::Format::eR8G8B8A8Unorm;
			case 3: return vk::Format::eR8G8B8Unorm;
			case 2: return vk::Format::eR8G8Unorm;
			case 1: return vk::Format::eR8Unorm;
			}
		} else {
			switch ( d.vecsize ) {
			case 4: return vk::Format::eR8G8B8A8Uint;
			case 3: return vk::Format::eR8G8B8Uint;
			case 2: return vk::Format::eR8G8Uint;
			case 1: return vk::Format::eR8Uint;
			}
		}
	    break;
	}

	assert(false); // abandon all hope
	return vk::Format::eUndefined;
}
// clang-format on

// Converts a le shader stage enum to a vulkan shader stage flag bit
// Currently these are kept in sync which means conversion is a simple
// matter of initialising one from the other.
static inline vk::ShaderStageFlagBits vk_to_le( const le::ShaderStage &stage ) {
	return vk::ShaderStageFlagBits( stage );
}

// ----------------------------------------------------------------------
// Creates a vulkan Pipeline based on a shader state object and a given renderpass and subpass index.
//
// TODO: Check spec whether shader stages must be sorted by stage flags (vertex first for example)
// when fed into the create info object. If so, we should make sure to sort befor building the object.
static vk::Pipeline le_pipeline_cache_create_pipeline( le_pipeline_manager_o *self, graphics_pipeline_state_o const *pso, const LeRenderPass &pass, uint32_t subpass ) {

	std::vector<vk::PipelineShaderStageCreateInfo> pipelineStages;
	pipelineStages.reserve( pso->shaderStages.size() );

	le_shader_module_o *vertexShaderModule = nullptr; // We may need the vertex shader module later

	for ( auto const &s : pso->shaderStages ) {

		// Try to set the vertex shader module pointer while we are at it. We will need it
		// when figuring out default bindings later, as the Vertex module is used to derive
		// default attribute bindings.
		if ( s->stage == le::ShaderStage::eVertex ) {
			vertexShaderModule = s;
		}

		vk::PipelineShaderStageCreateInfo info{};
		info
		    .setFlags( {} )                    // must be 0 - "reserved for future use"
		    .setStage( vk_to_le( s->stage ) )  //
		    .setModule( s->module )            //
		    .setPName( "main" )                //
		    .setPSpecializationInfo( nullptr ) //
		    ;

		pipelineStages.emplace_back( info );
	}

	std::vector<vk::VertexInputBindingDescription>   vertexBindingDescriptions;        // Where to get data from
	std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions; // How it feeds into the shader's vertex inputs

	if ( pso->explicitVertexInputBindingDescriptions.empty() ) {
		// Default: use vertex input schema based on shader reflection
		vertexBindingDescriptions        = vertexShaderModule->vertexBindingDescriptions;
		vertexInputAttributeDescriptions = vertexShaderModule->vertexAttributeDescriptions;
	} else {
		// use vertex input schema based on explicit user input
		// which was stored in `backend_create_grapics_pipeline_state_object`
		vertexBindingDescriptions.reserve( pso->explicitVertexInputBindingDescriptions.size() );
		vertexInputAttributeDescriptions.reserve( pso->explicitVertexAttributeDescriptions.size() );

		// create vertex input binding descriptions
		for ( auto const &b : pso->explicitVertexInputBindingDescriptions ) {

			vk::VertexInputBindingDescription bindingDescription;
			bindingDescription
			    .setBinding( b.binding )
			    .setStride( b.stride )
			    .setInputRate( vk_input_rate_from_le_input_rate( b.input_rate ) );

			vertexBindingDescriptions.emplace_back( std::move( bindingDescription ) );
		}

		for ( auto const &a : pso->explicitVertexAttributeDescriptions ) {
			vk::VertexInputAttributeDescription attributeDescription;
			attributeDescription
			    .setLocation( a.location )
			    .setBinding( a.binding )
			    .setOffset( a.binding_offset )
			    .setFormat( vk_format_from_le_vertex_input_attribute_description( a ) );

			vertexInputAttributeDescriptions.emplace_back( std::move( attributeDescription ) );
		}
	}

	// Combine vertex input `binding` state and vertex input `attribute` state into
	// something that vk will accept
	vk::PipelineVertexInputStateCreateInfo vertexInputStageInfo;
	vertexInputStageInfo
	    .setFlags( vk::PipelineVertexInputStateCreateFlags() )
	    .setVertexBindingDescriptionCount( uint32_t( vertexBindingDescriptions.size() ) )
	    .setPVertexBindingDescriptions( vertexBindingDescriptions.data() )
	    .setVertexAttributeDescriptionCount( uint32_t( vertexInputAttributeDescriptions.size() ) )
	    .setPVertexAttributeDescriptions( vertexInputAttributeDescriptions.data() ) //
	    ;

	// Fetch vk::PipelineLayout for this pso
	auto pipelineLayout = le_pipeline_manager_get_pipeline_layout( self, pso->shaderStages.data(), pso->shaderStages.size() );

	//
	// We must match blend attachment states with number of attachments for
	// the current renderpass - each attachment may have their own blend state.
	// Our pipeline objects will have 16 stages which are readable.
	//
	assert( pass.numColorAttachments <= VK_MAX_COLOR_ATTACHMENTS );
	//
	vk::PipelineColorBlendStateCreateInfo colorBlendState;
	colorBlendState
	    .setLogicOpEnable( VK_FALSE )
	    .setLogicOp( ::vk::LogicOp::eClear )
	    .setAttachmentCount( pass.numColorAttachments )
	    .setPAttachments( pso->data.blendAttachmentStates.data() )
	    .setBlendConstants( {{0.f, 0.f, 0.f, 0.f}} );

	// Viewport and Scissor are tracked as dynamic states, and although this object will not
	// get used, we must still fulfill the contract of providing a valid object to vk.
	//
	static vk::PipelineViewportStateCreateInfo defaultViewportState{vk::PipelineViewportStateCreateFlags(), 1, nullptr, 1, nullptr};

	// We will allways keep Scissor, Viewport and LineWidth as dynamic states,
	// otherwise we might have way too many pipelines flying around.
	std::array<vk::DynamicState, 3> dynamicStates = {{
	    vk::DynamicState::eScissor,
	    vk::DynamicState::eViewport,
	    vk::DynamicState::eLineWidth,
	}};

	vk::PipelineDynamicStateCreateInfo dynamicState;
	dynamicState
	    .setDynamicStateCount( dynamicStates.size() )
	    .setPDynamicStates( dynamicStates.data() );

	// setup pipeline
	vk::GraphicsPipelineCreateInfo gpi;
	gpi
	    .setFlags( vk::PipelineCreateFlagBits::eAllowDerivatives ) //
	    .setStageCount( uint32_t( pipelineStages.size() ) )        // set shaders
	    .setPStages( pipelineStages.data() )                       // set shaders
	    .setPVertexInputState( &vertexInputStageInfo )             //
	    .setPInputAssemblyState( &pso->data.inputAssemblyState )   //
	    .setPTessellationState( &pso->data.tessellationState )     //
	    .setPViewportState( &defaultViewportState )                // not used as these states are dynamic, defaultState is a dummy value to pacify driver
	    .setPRasterizationState( &pso->data.rasterizationInfo )    //
	    .setPMultisampleState( &pso->data.multisampleState )       //
	    .setPDepthStencilState( &pso->data.depthStencilState )     //
	    .setPColorBlendState( &colorBlendState )                   //
	    .setPDynamicState( &dynamicState )                         //
	    .setLayout( pipelineLayout )                               //
	    .setRenderPass( pass.renderPass )                          // must be a valid renderpass.
	    .setSubpass( subpass )                                     //
	    .setBasePipelineHandle( nullptr )                          //
	    .setBasePipelineIndex( 0 )                                 // -1 signals not to use a base pipeline index
	    ;

	auto pipeline = self->device.createGraphicsPipeline( self->vulkanCache, gpi );
	return pipeline;
}

// ----------------------------------------------------------------------

static vk::Pipeline le_pipeline_cache_create_compute_pipeline( le_pipeline_manager_o *self, compute_pipeline_state_o const *pso ) {

	// Fetch vk::PipelineLayout for this pso
	auto pipelineLayout = le_pipeline_manager_get_pipeline_layout( self, &pso->shaderStage, 1 );

	vk::PipelineShaderStageCreateInfo shaderStage{};
	shaderStage
	    .setFlags( {} )                                  // must be 0 - "reserved for future use"
	    .setStage( vk_to_le( pso->shaderStage->stage ) ) //
	    .setModule( pso->shaderStage->module )           //
	    .setPName( "main" )                              //
	    .setPSpecializationInfo( nullptr )               // TODO: Use specialisation consts to set workgroup size?
	    ;

	vk::ComputePipelineCreateInfo cpi;
	cpi
	    .setFlags( vk::PipelineCreateFlagBits::eAllowDerivatives )
	    .setStage( shaderStage )
	    .setLayout( pipelineLayout )
	    .setBasePipelineHandle( nullptr )
	    .setBasePipelineIndex( 0 ) // -1 signals not to use base pipeline index
	    ;

	auto pipeline = self->device.createComputePipeline( self->vulkanCache, cpi );
	return pipeline;
}

// ----------------------------------------------------------------------

/// \brief returns hash key for given bindings, creates and retains new vkDescriptorSetLayout inside backend if necessary
static uint64_t le_pipeline_cache_produce_descriptor_set_layout( le_pipeline_manager_o *self, std::vector<le_shader_binding_info> const &bindings, vk::DescriptorSetLayout *layout ) {

	auto &descriptorSetLayouts = self->descriptorSetLayouts; // FIXME: this method only needs rw access to this, and the device

	// -- Calculate hash based on le_shader_binding_infos for this set
	uint64_t set_layout_hash = SpookyHash::Hash64( bindings.data(), bindings.size() * sizeof( le_shader_binding_info ), 0 );

	auto foundLayout = descriptorSetLayouts.find( set_layout_hash );

	if ( foundLayout != descriptorSetLayouts.end() ) {

		// -- Layout was found in cache, reuse it.
		*layout = foundLayout->second.vk_descriptor_set_layout;

	} else {

		// -- Layout was not found in cache, we must create vk objects.

		std::vector<vk::DescriptorSetLayoutBinding> vk_bindings;

		vk_bindings.reserve( bindings.size() );

		for ( const auto &b : bindings ) {
			vk::DescriptorSetLayoutBinding binding{};
			binding.setBinding( b.binding )
			    .setDescriptorType( vk::DescriptorType( b.type ) )
			    .setDescriptorCount( b.count )
			    .setStageFlags( vk::ShaderStageFlags( b.stage_bits ) )
			    .setPImmutableSamplers( nullptr );
			vk_bindings.emplace_back( std::move( binding ) );
		}

		vk::DescriptorSetLayoutCreateInfo setLayoutInfo;
		setLayoutInfo
		    .setFlags( vk::DescriptorSetLayoutCreateFlags() )
		    .setBindingCount( uint32_t( vk_bindings.size() ) )
		    .setPBindings( vk_bindings.data() );

		*layout = self->device.createDescriptorSetLayout( setLayoutInfo );

		// -- Create descriptorUpdateTemplate
		//
		// The template needs to be created so that data for a vk::DescriptorSet
		// can be read from a vector of tightly packed
		// DescriptorData elements.
		//

		vk::DescriptorUpdateTemplate updateTemplate;
		{
			std::vector<vk::DescriptorUpdateTemplateEntry> entries;

			entries.reserve( bindings.size() );

			size_t base_offset = 0; // offset in bytes into DescriptorData vector, assuming vector is tightly packed.
			for ( const auto &b : bindings ) {
				vk::DescriptorUpdateTemplateEntry entry;

				auto descriptorType = vk::DescriptorType( b.type );

				entry.setDstBinding( b.binding );
				entry.setDescriptorCount( b.count );
				entry.setDescriptorType( descriptorType );
				entry.setDstArrayElement( 0 ); // starting element at this binding to update - always 0

				// set offset based on type of binding, so that template reads from correct data

				switch ( descriptorType ) {
				case vk::DescriptorType::eSampler:
				case vk::DescriptorType::eCombinedImageSampler:
				case vk::DescriptorType::eSampledImage:
				case vk::DescriptorType::eStorageImage:
				case vk::DescriptorType::eUniformTexelBuffer:
				case vk::DescriptorType::eStorageTexelBuffer:
				case vk::DescriptorType::eInputAttachment:
					// TODO: Find out what descriptorData an InputAttachment expects, if it is really done with an imageInfo
					entry.setOffset( base_offset + offsetof( DescriptorData, sampler ) ); // point to first field of ImageInfo
				    break;
				case vk::DescriptorType::eUniformBuffer:
				case vk::DescriptorType::eStorageBuffer:
				case vk::DescriptorType::eUniformBufferDynamic:
				case vk::DescriptorType::eStorageBufferDynamic:
					entry.setOffset( base_offset + offsetof( DescriptorData, buffer ) ); // point to first element of BufferInfo
				    break;
				}

				entry.setStride( sizeof( DescriptorData ) );

				entries.emplace_back( std::move( entry ) );

				base_offset += sizeof( DescriptorData );
			}

			vk::DescriptorUpdateTemplateCreateInfo info;
			info
			    .setFlags( {} ) // no flags for now
			    .setDescriptorUpdateEntryCount( uint32_t( entries.size() ) )
			    .setPDescriptorUpdateEntries( entries.data() )
			    .setTemplateType( vk::DescriptorUpdateTemplateType::eDescriptorSet )
			    .setDescriptorSetLayout( *layout )
			    .setPipelineBindPoint( {} ) // ignored for this template type
			    .setPipelineLayout( {} )    // ignored for this template type
			    .setSet( 0 )                // ignored for this template type
			    ;

			updateTemplate = self->device.createDescriptorUpdateTemplate( info );
		}

		le_descriptor_set_layout_t le_layout_info;
		le_layout_info.vk_descriptor_set_layout      = *layout;
		le_layout_info.binding_info                  = bindings;
		le_layout_info.vk_descriptor_update_template = updateTemplate;

		descriptorSetLayouts[ set_layout_hash ] = std::move( le_layout_info );
	}

	return set_layout_hash;
}

// ----------------------------------------------------------------------
// Calculates pipeline layout info by first consolidating all bindings
// over all referenced modules, and then ordering these by descriptor sets.
//
static le_pipeline_layout_info le_pipeline_cache_produce_pipeline_layout_info( le_pipeline_manager_o *self, le_shader_module_o const *const *shader_modules, size_t shader_modules_count ) {
	le_pipeline_layout_info info{};

	std::vector<le_shader_binding_info> combined_bindings = shader_modules_get_bindings_list( shader_modules, shader_modules_count );

	// -- Create array of DescriptorSetLayouts
	std::array<vk::DescriptorSetLayout, 8> vkLayouts{};
	{

		// -- Create one vkDescriptorSetLayout for each set in bindings

		std::vector<std::vector<le_shader_binding_info>> sets;

		// Split combined bindings at set boundaries
		uint32_t set_idx = 0;
		for ( auto it = combined_bindings.begin(); it != combined_bindings.end(); ) {

			// Find next element with different set id
			auto itN = std::find_if( it, combined_bindings.end(), [&set_idx]( const le_shader_binding_info &el ) -> bool {
				return el.setIndex != set_idx;
			} );

			sets.emplace_back( it, itN );

			// If we're not at the end, get the setIndex for the next set,
			if ( itN != combined_bindings.end() ) {
				assert( set_idx + 1 == itN->setIndex ); // we must enforce that sets are non-sparse.
				set_idx = itN->setIndex;
			}

			it = itN;
		}

		info.set_layout_count = uint32_t( sets.size() );
		assert( sets.size() <= VK_MAX_BOUND_DESCRIPTOR_SETS ); // must be less or equal to maximum bound descriptor sets (currently 8 on NV)

		{
			// Assert that sets and bindings are sparse (you must not have "holes" in sets, bindings.)
			// FIXME: (check-shader-bindings) we must find a way to recover from this, but it might be difficult without a "linking" stage
			// which combines various shader stages.
			set_idx = 0;
			for ( auto const &s : sets ) {
				uint32_t binding = 0;
				for ( auto const &b : s ) {
					assert( b.binding == binding );
					assert( b.setIndex == set_idx );
					binding++;
				}
				set_idx++;
			}
		}

		for ( size_t i = 0; i != sets.size(); ++i ) {
			info.set_layout_keys[ i ] = le_pipeline_cache_produce_descriptor_set_layout( self, sets[ i ], &vkLayouts[ i ] );
		}
	}

	info.pipeline_layout_key = shader_modules_get_pipeline_layout_hash( shader_modules, shader_modules_count );

	// -- Attempt to find this pipelineLayout from cache, if we can't find one, we create and retain it.

	auto found_pl = self->pipelineLayouts.find( info.pipeline_layout_key );

	if ( found_pl == self->pipelineLayouts.end() ) {

		vk::Device                   device = self->device;
		vk::PipelineLayoutCreateInfo layoutCreateInfo;
		layoutCreateInfo
		    .setFlags( vk::PipelineLayoutCreateFlags() ) // "reserved for future use"
		    .setSetLayoutCount( uint32_t( info.set_layout_count ) )
		    .setPSetLayouts( vkLayouts.data() )
		    .setPushConstantRangeCount( 0 )
		    .setPPushConstantRanges( nullptr );

		// Create vkPipelineLayout and store it in cache.
		self->pipelineLayouts[ info.pipeline_layout_key ] = device.createPipelineLayout( layoutCreateInfo );
	}

	return info;
}

// ----------------------------------------------------------------------
/// \returns pointer to a graphicsPSO which matches gpsoHash, or `nullptr` if no match
graphics_pipeline_state_o *le_pipeline_manager_get_gpso_from_cache( le_pipeline_manager_o *self, const le_gpso_handle &gpso_hash ) {
	// FIXME: (PIPELINE) THIS NEEDS TO BE MUTEXED, AND ACCESS CONTROLLED

	auto       pso              = self->graphicsPSO_list.data();
	const auto pso_hashes_begin = self->graphicsPSO_handles.data();
	auto       pso_hash         = pso_hashes_begin;
	const auto pso_hashes_end   = pso_hashes_begin + self->graphicsPSO_handles.size();

	for ( ; pso_hash != pso_hashes_end; pso_hash++, pso++ ) {
		if ( gpso_hash == *pso_hash )
			break;
	}

	if ( pso_hash == pso_hashes_end ) {
		// not found
		return nullptr; // could not find pso with given hash
	}

	// ---------| invariant: element found

	return *pso;
}

// ----------------------------------------------------------------------
/// \returns pointer to a computePSO which matches cpsoHash, or `nullptr` if no match
compute_pipeline_state_o *le_pipeline_manager_get_cpso_from_cache( le_pipeline_manager_o *self, const le_cpso_handle &cpso_hash ) {
	// FIXME: (PIPELINE) THIS NEEDS TO BE MUTEXED, AND ACCESS CONTROLLED

	auto       pso              = self->computePSO_list.data();
	const auto pso_hashes_begin = self->computePSO_handles.data();
	auto       pso_hash         = pso_hashes_begin;
	const auto pso_hashes_end   = pso_hashes_begin + self->computePSO_handles.size();

	for ( ; pso_hash != pso_hashes_end; pso_hash++, pso++ ) {
		if ( cpso_hash == *pso_hash )
			break;
	}

	if ( pso_hash == pso_hashes_end ) {
		// not found
		return nullptr; // could not find pso with given hash
	}

	// ---------| invariant: element found

	return *pso;
}

// ----------------------------------------------------------------------

/// \brief Creates - or loads a pipeline from cache - based on current pipeline state
/// \note This method may lock the pipeline cache and is therefore costly.
// TODO: Ensure there are no races around this method
//
// + Only the 'command buffer recording'-slice of a frame shall be able to modify the cache.
//   The cache must be exclusively accessed through this method
//
// + Access to this method must be sequential - no two frames may access this method
//   at the same time - and no two renderpasses may access this method at the same time.
static le_pipeline_and_layout_info_t le_pipeline_manager_produce_pipeline( le_pipeline_manager_o *self, le_gpso_handle gpso_handle, const LeRenderPass &pass, uint32_t subpass ) {

	// -- 0. Fetch pso from cache using its hash key

	graphics_pipeline_state_o const *pso = le_pipeline_manager_get_gpso_from_cache( self, gpso_handle );

	assert( pso );

	le_pipeline_and_layout_info_t pipeline_and_layout_info = {};

	// -- 1. get pipeline layout info for a pipeline with these bindings
	// we try to fetch it from the cache first, if it doesn't exist, we must create it, and add it to the cache.

	uint64_t pipeline_layout_hash = shader_modules_get_pipeline_layout_hash( pso->shaderStages.data(), pso->shaderStages.size() );

	auto pl = self->pipelineLayoutInfos.find( pipeline_layout_hash );

	if ( pl == self->pipelineLayoutInfos.end() ) {

		// this will also create vulkan objects for pipeline layout / descriptor set layout and cache them
		pipeline_and_layout_info.layout_info = le_pipeline_cache_produce_pipeline_layout_info( self, pso->shaderStages.data(), pso->shaderStages.size() );

		// store in cache
		self->pipelineLayoutInfos[ pipeline_layout_hash ] = pipeline_and_layout_info.layout_info;
	} else {
		pipeline_and_layout_info.layout_info = pl->second;
	}

	// -- 2. get vk pipeline object
	// we try to fetch it from the cache first, if it doesn't exist, we must create it, and add it to the cache.

	uint64_t pipeline_hash = 0;
	{
		// Create a combined hash for pipeline, renderpass, and all contributing shader stages.

		uint64_t pso_renderpass_hash_data[ 12 ]       = {}; // we use a c-style array, with an entry count so that this is reliably allocated on the stack and not on the heap.
		uint64_t pso_renderpass_hash_data_num_entries = 0;  // number of entries in pso_renderpass_hash_data

		pso_renderpass_hash_data[ 0 ]        = reinterpret_cast<uint64_t>( gpso_handle ); // Hash associated with `pso`
		pso_renderpass_hash_data[ 1 ]        = pass.renderpassHash;                       // Hash for *compatible* renderpass
		pso_renderpass_hash_data_num_entries = 2;

		for ( auto const &s : pso->shaderStages ) {
			pso_renderpass_hash_data[ pso_renderpass_hash_data_num_entries++ ] = s->hash; // Module state - may have been recompiled, hash must be current
		}

		// -- create combined hash for pipeline, renderpass
		pipeline_hash = SpookyHash::Hash64( pso_renderpass_hash_data, sizeof( uint64_t ) * pso_renderpass_hash_data_num_entries, pipeline_layout_hash );
	}

	// -- look up if pipeline with this hash already exists in cache
	auto p = self->pipelines.find( pipeline_hash );

	if ( p == self->pipelines.end() ) {

		// -- if not, create pipeline in pipeline cache and store / retain it
		pipeline_and_layout_info.pipeline = le_pipeline_cache_create_pipeline( self, pso, pass, subpass );

		std::cout << "New VK Graphics Pipeline created: 0x" << std::hex << pipeline_hash << std::endl
		          << std::flush;

		self->pipelines[ pipeline_hash ] = pipeline_and_layout_info.pipeline;
	} else {
		// -- else return pipeline found in hash map
		pipeline_and_layout_info.pipeline = p->second;
	}

	return pipeline_and_layout_info;
}

// ----------------------------------------------------------------------

static le_pipeline_and_layout_info_t le_pipeline_manager_produce_compute_pipeline( le_pipeline_manager_o *self, le_cpso_handle cpso_handle ) {

	compute_pipeline_state_o const *pso = le_pipeline_manager_get_cpso_from_cache( self, cpso_handle );
	assert( pso );

	le_pipeline_and_layout_info_t pipeline_and_layout_info = {};

	// Fetch the hash over the pipeline layout. Since there is only one shader
	// stage for compute, we can take it straight from that stage without
	// accumulating over stages.
	uint64_t pipeline_layout_hash = shader_modules_get_pipeline_layout_hash( &pso->shaderStage, 1 );

	auto pl = self->pipelineLayoutInfos.find( pipeline_layout_hash );

	if ( pl == self->pipelineLayoutInfos.end() ) {

		// this will also create vulkan objects for pipeline layout / descriptor set layout and cache them
		pipeline_and_layout_info.layout_info = le_pipeline_cache_produce_pipeline_layout_info( self, &pso->shaderStage, 1 );

		// store in cache
		self->pipelineLayoutInfos[ pipeline_layout_hash ] = pipeline_and_layout_info.layout_info;
	} else {
		pipeline_and_layout_info.layout_info = pl->second;
	}

	// -- 2. get vk pipeline object
	// we try to fetch it from the cache first, if it doesn't exist, we must create it, and add it to the cache.

	uint64_t pipeline_hash = 0;
	{
		// Create a combined hash for pipeline, renderpass, and all contributing shader stages.

		// We use a fixed-size c-style array to collect all hashes for this pipeline,
		// and an entry count so that this is reliably allocated on the stack and not on the heap.
		//
		uint64_t pso_renderpass_hash_data[ 12 ]       = {};
		uint64_t pso_renderpass_hash_data_num_entries = 0; // max 12

		pso_renderpass_hash_data[ pso_renderpass_hash_data_num_entries++ ] = reinterpret_cast<uint64_t>( cpso_handle ); // Hash associated with `pso`
		pso_renderpass_hash_data[ pso_renderpass_hash_data_num_entries++ ] = pso->shaderStage->hash;                    // Module state - may have been recompiled, hash must be current

		// -- create combined hash for pipeline, renderpass
		pipeline_hash = SpookyHash::Hash64( pso_renderpass_hash_data, sizeof( uint64_t ) * pso_renderpass_hash_data_num_entries, pipeline_layout_hash );
	}

	// -- look up if pipeline with this hash already exists in cache.
	auto p = self->pipelines.find( pipeline_hash );

	if ( p == self->pipelines.end() ) {

		// -- if not, create pipeline in pipeline cache and store / retain it
		pipeline_and_layout_info.pipeline = le_pipeline_cache_create_compute_pipeline( self, pso );

		std::cout << "New VK Compute Pipeline created: 0x" << std::hex << pipeline_hash << std::endl
		          << std::flush;

		self->pipelines[ pipeline_hash ] = pipeline_and_layout_info.pipeline;
	} else {
		// -- else return pipeline found in hash map
		pipeline_and_layout_info.pipeline = p->second;
	}

	return pipeline_and_layout_info;
}

// ----------------------------------------------------------------------
// This method may get called through the pipeline builder -
//
// via RECORD in command buffer recording state
// in SETUP
void le_pipeline_manager_introduce_graphics_pipeline_state( le_pipeline_manager_o *self, graphics_pipeline_state_o *gpso, le_gpso_handle gpsoHandle ) {

	// we must copy!

	// check if pso is already in cache
	auto pso = le_pipeline_manager_get_gpso_from_cache( self, gpsoHandle );

	if ( pso == nullptr ) {
		// not found in cache - add to cache
		self->graphicsPSO_handles.emplace_back( gpsoHandle );
		self->graphicsPSO_list.emplace_back( new graphics_pipeline_state_o( *gpso ) ); // note that we copy
	} else {
		// assert( false ); // pso was already found in cache, this is strange
	}
};

// ----------------------------------------------------------------------
// This method may get called through the pipeline builder -
//
// via RECORD in command buffer recording state
// in SETUP
void le_pipeline_manager_introduce_compute_pipeline_state( le_pipeline_manager_o *self, compute_pipeline_state_o *cpso, le_cpso_handle cpsoHandle ) {

	// we must copy!

	// check if pso is already in cache
	auto pso = le_pipeline_manager_get_cpso_from_cache( self, cpsoHandle );

	if ( pso == nullptr ) {
		// not found in cache - add to cache
		self->computePSO_handles.emplace_back( cpsoHandle );
		self->computePSO_list.emplace_back( new compute_pipeline_state_o( *cpso ) ); // note that we copy
	} else {
		// assert( false ); // pso was already found in cache, this is strange
	}
};

// ----------------------------------------------------------------------

static VkPipelineLayout le_pipeline_manager_get_pipeline_layout( le_pipeline_manager_o *self, uint64_t key ) {
	return self->pipelineLayouts[ key ];
}

// ----------------------------------------------------------------------

static const le_descriptor_set_layout_t &le_pipeline_manager_get_descriptor_set_layout( le_pipeline_manager_o *self, uint64_t setlayout_key ) {
	return self->descriptorSetLayouts[ setlayout_key ];
};

// ----------------------------------------------------------------------

static le_shader_module_o *le_pipeline_manager_create_shader_module( le_pipeline_manager_o *self, char const *path, const LeShaderStageEnum &moduleType ) {
	return le_shader_manager_create_shader_module( self->shaderManager, path, moduleType );
}

// ----------------------------------------------------------------------

static void le_pipeline_manager_update_shader_modules( le_pipeline_manager_o *self ) {
	le_shader_manager_update_shader_modules( self->shaderManager );
}

// ----------------------------------------------------------------------

static le_pipeline_manager_o *le_pipeline_manager_create( VkDevice_T *device ) {
	auto self    = new le_pipeline_manager_o();
	self->device = device;

	vk::PipelineCacheCreateInfo pipelineCacheInfo;
	pipelineCacheInfo
	    .setFlags( vk::PipelineCacheCreateFlags() ) // "reserved for future use"
	    .setInitialDataSize( 0 )
	    .setPInitialData( nullptr );

	self->vulkanCache = self->device.createPipelineCache( pipelineCacheInfo );

	self->shaderManager = le_shader_manager_create( device );

	return self;
}

// ----------------------------------------------------------------------

static void le_pipeline_manager_destroy( le_pipeline_manager_o *self ) {

	le_shader_manager_destroy( self->shaderManager );
	self->shaderManager = nullptr;

	// -- destroy any pipeline state objects
	self->graphicsPSO_handles.clear();
	for ( auto &pPso : self->graphicsPSO_list ) {
		delete ( pPso );
	}
	self->graphicsPSO_list.clear();

	// -- destroy renderpasses

	// -- destroy descriptorSetLayouts

	std::cout << "Destroying " << self->descriptorSetLayouts.size() << " DescriptorSetLayouts" << std::endl
	          << std::flush;
	for ( auto &p : self->descriptorSetLayouts ) {
		self->device.destroyDescriptorSetLayout( p.second.vk_descriptor_set_layout );
		self->device.destroyDescriptorUpdateTemplate( p.second.vk_descriptor_update_template );
	}

	// -- destroy pipelineLayouts
	std::cout << "Destroying " << self->pipelineLayouts.size() << " PipelineLayouts" << std::endl
	          << std::flush;
	for ( auto &l : self->pipelineLayouts ) {
		self->device.destroyPipelineLayout( l.second );
	}

	for ( auto &p : self->pipelines ) {
		self->device.destroyPipeline( p.second );
	}
	self->pipelines.clear();

	if ( self->vulkanCache ) {
		self->device.destroyPipelineCache( self->vulkanCache );
	}

	delete self;
}

// ----------------------------------------------------------------------

ISL_API_ATTR void register_le_pipeline_vk_api( void *api_ ) {

	auto le_backend_vk_api_i = static_cast<le_backend_vk_api *>( api_ );
	{
		auto &i = le_backend_vk_api_i->le_pipeline_manager_i;

		i.create  = le_pipeline_manager_create;
		i.destroy = le_pipeline_manager_destroy;

		i.create_shader_module              = le_pipeline_manager_create_shader_module;
		i.update_shader_modules             = le_pipeline_manager_update_shader_modules;
		i.introduce_graphics_pipeline_state = le_pipeline_manager_introduce_graphics_pipeline_state;
		i.introduce_compute_pipeline_state  = le_pipeline_manager_introduce_compute_pipeline_state;
		i.get_pipeline_layout               = le_pipeline_manager_get_pipeline_layout;
		i.get_descriptor_set_layout         = le_pipeline_manager_get_descriptor_set_layout;
		i.produce_pipeline                  = le_pipeline_manager_produce_pipeline;
		i.produce_compute_pipeline          = le_pipeline_manager_produce_compute_pipeline;
	}
	{
		auto &i     = le_backend_vk_api_i->le_shader_module_i;
		i.get_hash  = le_shader_module_get_hash;
		i.get_stage = le_shader_module_get_stage;
	}
}
