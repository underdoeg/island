#include "le_file_watcher.h"

#ifdef LE_FILE_WATCHER_IMPL_WIN32

#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>

#	include <iomanip>
#	include <iostream>
#	include <bitset>
#	include <stdio.h>
#	include <string>
#	include <list>
#	include <filesystem>
#	include <algorithm>
#	include <array>
#	include "le_core/hash_util.h"

/*

TODO:

While this works for a rough POC, there are a couple of issues with it: 

While on linux, creating a watch for a directory which is already watched does not create another watch,
on windows each watch is created, and will get triggered. Linux uses an internal cache for watches. We
should do so, too.

Since our api watches files, this means that we end up with a trigger for each file 
which is in a directory, which is not what we want, because the number of messages explodes as soon 
as we have plenty of files per directory.

We want an architecture where we internally have a list of watched directories, and each directory has 
a list of watched files. If a directory triggers, we find if the file that did the trigger is on our
list of watched files, and if it is, we trigger the callback.


*/

// ----------------------------------------------------------------------

struct WatchedDirectory {
	OVERLAPPED                 overlapped          = {}; // imortant that this is the first element, so that we may cast.
	std::array<BYTE, 4096 * 4> buffer              = {}; // 4 pages
	HANDLE                     directory_handle    = nullptr;
	DWORD                      notify_filter       = 0;
	uint64_t                   directory_name_hash = 0;
	struct le_file_watcher_o * watcher             = nullptr; // non-owning, refers back to parent

	~WatchedDirectory() {
		if ( overlapped.hEvent ) {
			CloseHandle( overlapped.hEvent );
		}
		if ( directory_handle ) {
			CloseHandle( directory_handle );
		}
	}
};

struct watch_data_t {
	std::string path;
	std::string filename;
	std::string basename;
	int         handle             = -1; // per-file_watcher unique handle
	void *      callback_user_data = nullptr;
	bool ( *callback_fun )( const char *path, void *user_data );
};

// ----------------------------------------------------------------------

struct le_file_watcher_o {
	int last_watch_handle = -1; // monotonically increases just before we create a new watch_id - *not an index* into vectors below.

	std::vector<uint64_t>                  watch_directory_hash; // < these three vectors run in parallel.
	std::vector<WatchedDirectory *>        watched_directories;  // < these three vectors run in parallel.
	std::vector<std::vector<watch_data_t>> watch_data;           // < these three vectors run in parallel.

	/// Returns index of watch_data for given hash.
	/// If not found, index will be watch_data.size()
	inline size_t get_watch_data_idx_for_hash( uint64_t hash ) const {
		size_t idx = 0;
		for ( auto const &w : watch_directory_hash ) {
			if ( w == hash ) {
				return idx;
			}
		}
		return idx;
	};
};

// ----------------------------------------------------------------------

static le_file_watcher_o *file_watcher_instance_create() {
	auto instance = new le_file_watcher_o();

	// todo: implement

	return instance;
}

static void refresh_watch( WatchedDirectory *w ); // ffdecl

// ----------------------------------------------------------------------

static void file_watcher_instance_destroy( le_file_watcher_o *instance ) {

	// close all watch objects in instance
	// destroy all watch objects in instance
	// remove all watch objects from instance
	// destroy instance

	for ( auto &w : instance->watched_directories ) {
		delete w;
	}

	instance->watched_directories.clear();

	delete ( instance );
}

// ----------------------------------------------------------------------
/// \brief add a watch based on a particular file path
static int file_watcher_add_watch( le_file_watcher_o *instance, le_file_watcher_watch_settings const *settings ) noexcept {

	// First we want to find out if there is already a watch for the directory
	auto file_path = std::filesystem::canonical( settings->filePath );

	std::cout << "Adding watch for: '" << file_path.string() << "'" << std::endl
	          << std::flush;

	auto           file_name          = file_path.filename().string();
	auto           file_basename      = std::filesystem::path( file_path ).remove_filename().string();
	uint64_t const file_basename_hash = hash_64_fnv1a( file_basename.c_str() );

	size_t watch_index = 0;

	watch_index = instance->get_watch_data_idx_for_hash( file_basename_hash );

	if ( watch_index == instance->watch_directory_hash.size() ) {

		// -------| invariant: there is not yet a matching watch for this directory

		// we must add a new watch.
		auto watched_directory = new WatchedDirectory(); // zero-initialise everything.

		watched_directory->directory_name_hash = file_basename_hash;
		watched_directory->watcher             = instance;
		watched_directory->notify_filter       = FILE_NOTIFY_CHANGE_LAST_WRITE;

		watched_directory->directory_handle =
		    CreateFile( file_basename.c_str(), FILE_LIST_DIRECTORY,
		                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL );

		if ( watched_directory->directory_handle != INVALID_HANDLE_VALUE ) {
			watched_directory->overlapped.hEvent = CreateEvent( NULL, TRUE, FALSE, NULL );
		} else {
			// TODO: we must notify that something went wrong!
		}

		refresh_watch( watched_directory );

		instance->watch_directory_hash.push_back( file_basename_hash );
		instance->watched_directories.emplace_back( watched_directory );
		instance->watch_data.emplace_back(); // an empty entry to watch_data
	}

	// ---------| invariant: watch_index is the index of the correct watch for this directory.

	{
		watch_data_t watch_data{};
		watch_data.path               = file_path.string();
		watch_data.basename           = file_basename;
		watch_data.filename           = file_name;
		watch_data.callback_fun       = settings->callback_fun;
		watch_data.callback_user_data = settings->callback_user_data;
		int watch_handle              = ++instance->last_watch_handle;
		watch_data.handle             = watch_handle;
		instance->watch_data[ watch_index ].emplace_back( std::move( watch_data ) );

		// check if a watch_data for this file already exists - if it does,
		// we must not add another one - or should we?

		return watch_handle;
	}

	return -1;
}

// ----------------------------------------------------------------------
/// \brief remove watch given by watch_id
/// \return true on success, otherwise false.
static bool file_watcher_remove_watch( le_file_watcher_o *instance, int watch_id ) {

	size_t idx           = 0;
	size_t watch_idx     = 0;
	bool   found_element = false;

	for ( ; idx < instance->watch_data.size(); ) {

		auto &w_vec = instance->watch_data[ idx ];

		// Remove file watch from directory watch

		for ( watch_idx = 0; watch_idx < w_vec.size(); ) {

			if ( w_vec[ watch_idx ].handle == watch_id ) {
				w_vec.erase( w_vec.begin() + watch_idx );
				found_element = true;
				continue;
			} else {
				watch_idx++;
			}
		}

		// If no file watches remain for the current directory, remove the
		// directory, and close the directory watch.

		if ( w_vec.empty() ) {

			delete ( instance->watched_directories[ idx ] );

			instance->watched_directories.erase( instance->watched_directories.begin() + idx );
			instance->watch_directory_hash.erase( instance->watch_directory_hash.begin() + idx );
			instance->watch_data.erase( instance->watch_data.begin() + idx );

		} else {
			idx++;
		}

		// Once the elmement has been found we can return early, as
		// there is only ever one element with a given id.

		if ( found_element ) {
			return true;
		}
	}

	return false;
};

// ----------------------------------------------------------------------
// when a callback gets tiggered, we filter against all
// watchdata elements in the vector matching the directory hash
static void CALLBACK watch_callback( DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped ) {
	char                     callback_filename[ MAX_PATH ];
	PFILE_NOTIFY_INFORMATION pNotify;
	WatchedDirectory *       watch  = ( WatchedDirectory * )lpOverlapped;
	size_t                   offset = 0;

	if ( dwNumberOfBytesTransfered == 0 ) {
		return;
	}

	if ( dwErrorCode != ERROR_SUCCESS ) {
		return;
	}

	do {
		pNotify = ( PFILE_NOTIFY_INFORMATION )&watch->buffer[ offset ];
		offset += pNotify->NextEntryOffset;

		int count =
		    WideCharToMultiByte( CP_ACP, 0, pNotify->FileName,
		                         pNotify->FileNameLength / sizeof( WCHAR ),
		                         callback_filename, MAX_PATH - 1, NULL, NULL );

		callback_filename[ count ] = '\0';

		size_t idx = watch->watcher->get_watch_data_idx_for_hash( watch->directory_name_hash );

		if ( idx < watch->watcher->watch_data.size() ) {

			for ( auto const &w : watch->watcher->watch_data[ idx ] ) {
				// go through all watches and match against the file name
				if ( 0 == strncmp( callback_filename, w.filename.c_str(), count ) ) {
					( *w.callback_fun )( w.path.c_str(), w.callback_user_data );
				}
			}
		}

	} while ( pNotify->NextEntryOffset != 0 );

	// we must re-issue watch

	refresh_watch( watch );
}

static void refresh_watch( WatchedDirectory *watch_dir ) {

	ReadDirectoryChangesW( watch_dir->directory_handle,
	                       watch_dir->buffer.data(),
	                       DWORD( watch_dir->buffer.size() ),
	                       FALSE,
	                       watch_dir->notify_filter,
	                       NULL,
	                       &watch_dir->overlapped,
	                       watch_callback );
}

// ----------------------------------------------------------------------
/// \brief trigger callbacks on any watches which have pending notifications
///
static void file_watcher_poll_notifications( le_file_watcher_o * ) {
	MsgWaitForMultipleObjectsEx( 0, NULL, 0, QS_ALLINPUT, MWMO_ALERTABLE );
}

// ----------------------------------------------------------------------

LE_MODULE_REGISTER_IMPL( le_file_watcher, p_api ) {
	auto  api                = reinterpret_cast<le_file_watcher_api *>( p_api );
	auto &api_i              = api->le_file_watcher_i;
	api_i.create             = file_watcher_instance_create;
	api_i.destroy            = file_watcher_instance_destroy;
	api_i.add_watch          = file_watcher_add_watch;
	api_i.remove_watch       = file_watcher_remove_watch;
	api_i.poll_notifications = file_watcher_poll_notifications;
};

#endif