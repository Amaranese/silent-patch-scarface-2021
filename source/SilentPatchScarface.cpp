#include "Utils/MemoryMgr.h"
#include "Utils/Patterns.h"

#include "pure3d.h"

#include <set>

static void* (*orgMalloc)(size_t size);
void* scarMalloc( size_t size )
{
	return std::invoke( orgMalloc, size );
}

namespace INISettings
{
	bool WriteSetting( const char* key, int value )
	{
		char buffer[32];
		sprintf_s( buffer, "%d", value );
		return WritePrivateProfileStringA( "Scarface", key, buffer, ".\\settings.ini" ) != FALSE;
	}

	int ReadSetting( const char* key )
	{
		return GetPrivateProfileIntA( "Scarface", key, -1, ".\\settings.ini" );
	}
}

namespace ListAllResolutions
{
	static void (*orgInvokeScriptCommand)(const char* format, ...);

	static void** pUnknown3dStruct; // Used to get the D3D device
	static IDirect3D9* GetD3D()
	{
		// Ugly, but those structs are totally unknown at the moment
		void* ptr1 = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pUnknown3dStruct) + 4);
		void* d3dDisplay = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ptr1) + 232);
		if ( d3dDisplay != nullptr )
		{
			return *reinterpret_cast<IDirect3D9**>(reinterpret_cast<uintptr_t>(d3dDisplay) + 12);
		}
		return nullptr;
	}

	// Original function only lists a set of supported resolutions - list them all instead
	static void ListResolutions()
	{
		IDirect3D9* d3d = GetD3D();
		if ( d3d != nullptr )
		{
			// Filters out duplicates and sorts everything for us
			std::set< std::pair<UINT, UINT> > resolutionsSet;

			for ( UINT i = 0, j = d3d->GetAdapterModeCount( 0, D3DFMT_X8R8G8B8 ); i < j; i++ )
			{
				D3DDISPLAYMODE mode;
				if ( SUCCEEDED(d3d->EnumAdapterModes( 0, D3DFMT_X8R8G8B8, i, &mode )) )
				{
					resolutionsSet.emplace( mode.Width, mode.Height );
				}
			}

			for ( auto& it : resolutionsSet )
			{
				orgInvokeScriptCommand( "AddScreenResolutionEntry( %u, %u, %u );", it.second, it.first, 32 );
			}
		}
	}
}

// Stub for pure3d error checking
static void CheckRefCountStub( uint32_t /*count*/ )
{
}


void OnInitializeHook()
{
	using namespace Memory;
	using namespace hook;

	std::unique_ptr<ScopedUnprotect::Unprotect> Protect = ScopedUnprotect::UnprotectSectionOrFullModule( GetModuleHandle( nullptr ), ".text" );

	// Scarface malloc
	{
		auto alloc = get_pattern( "E8 ? ? ? ? 89 28" );
		ReadCall( alloc, orgMalloc );
	}


	// Remove D3DLOCK_DISCARD flag from vertex locks, as game makes false assumptions about its behaviour
	{
		auto vblock = get_pattern( "52 6A 00 6A 00 50 FF 51 2C 50 E8 ? ? ? ? BA", -5 + 1 );
		Patch<int32_t>( vblock, 0 );
	}

	// Allow the game to run on all cores
	{
		auto setAffinity = get_pattern( "56 8B 35 ? ? ? ? 8D 44 24 08", -3 );
		Patch( setAffinity, { 0xC3 } ); // retn
	}

	// Move settings to a settings.ini file in game directory (saves are already being stored there)
	{
		using namespace INISettings;

		auto write = get_pattern( "8D 44 24 2C 50 68 3F 00 0F 00 6A 00 51", -0x12 );
		InjectHook( write, WriteSetting, PATCH_JUMP );

		auto read = get_pattern( "83 EC 2C 8D 04 24 50", -6 );
		InjectHook( read, ReadSetting, PATCH_JUMP );
	}

	// Remove D3DCREATE_MULTITHREADED flag from device creation, as the game does not actually need it
	// and it might decrease performance
	{
		// This code changed between 1.0 and 1.00.2 and building one pattern for both is not feasible
		auto pattern10 = pattern( "51 8B 8D D8 01 00 00 6A 01 51 50 FF 52 40" ).count_hint(1); // 1.0
		if ( pattern10.size() == 1 )
		{
			Patch<int8_t>( pattern10.get_first( -2 + 1 ), 0x40 ); // D3DCREATE_HARDWARE_VERTEXPROCESSING
		}
		else
		{
			auto pattern1002 = pattern( "50 6A 01 57 51 FF 52 40" ).count_hint(1); // 1.00.2
			if ( pattern1002.size() == 1 )
			{
				Patch<int8_t>( pattern1002.get_first( -2 + 1 ), 0x40 ); // D3DCREATE_HARDWARE_VERTEXPROCESSING
			}
		}
	}

	// Pooled D3D vertex and index buffers for improve performance
	{
		gpPure3d = *get_pattern<pure3d**>( "FF 52 10 A1", 3 + 1 );

		auto createResources = pattern( "E8 ? ? ? ? 8B 4C 24 20 51 8B CE" ).get_one();
	
		auto vb = createResources.get<void>();
		ReadCall( vb, pure3d::d3dPrimBuffer::orgCreateVertexBuffer );
		InjectHook( vb, &pure3d::d3dPrimBuffer::GetOrCreateVertexBuffer );

		auto ib = createResources.get<void>( 0xC );
		ReadCall( ib, pure3d::d3dPrimBuffer::orgCreateIndexBuffer );
		InjectHook( ib, &pure3d::d3dPrimBuffer::GetOrCreateIndexBuffer );

		auto dtor = get_pattern( "8B 48 10 89 4E 0C 8B C6", 0xF + 3 );
		ReadCall( dtor, pure3d::d3dPrimBuffer::orgDtor );
		InjectHook( dtor, &pure3d::d3dPrimBuffer::ReclaimAndDestroy );

		auto freeMem = get_pattern( "E8 ? ? ? ? 0F B7 4E 18" );
		ReadCall( freeMem, pure3d::orgFreeMemory );

		auto deviceLost1 = get_pattern( "75 26 E8 ? ? ? ? 8B 86 ? ? ? ?", 2 );
		ReadCall( deviceLost1, pure3d::orgOnDeviceLost );
		InjectHook( deviceLost1, pure3d::FlushCachesOnDeviceLost );

		auto deviceLost2 = get_pattern( "E8 ? ? ? ? 8B 06 8B 08 53" );
		InjectHook( deviceLost2, pure3d::FlushCachesOnDeviceLost );

		// Remove a false positive from 1.0, where vb->Release() return value gets treated as HRESULT
		// and breaks with our cache
		pattern( "FF 52 08 50 E8 ? ? ? ? A1 ? ? ? ? 83 C4 04" ).for_each_result( []( pattern_match match ) {
			InjectHook( match.get<void>( 4 ), CheckRefCountStub );
		} );
	}

	// List all resolutions
	{
		using namespace ListAllResolutions;

		pUnknown3dStruct = *get_pattern<void**>( "FF D7 8B 0D ? ? ? ? 83 C4 0C", 2 + 2 );

		auto scriptCommand = get_pattern( "68 ? ? ? ? E8 ? ? ? ? 8B 53 14", 5 );
		ReadCall( scriptCommand, orgInvokeScriptCommand );

		auto listResolutions = get_pattern( "E8 ? ? ? ? A1 ? ? ? ? 85 C0 74 20" );
		InjectHook( listResolutions, ListResolutions );
	}
}