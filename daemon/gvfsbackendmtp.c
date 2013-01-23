/* GIO - GLib Input, Output and Streaming Library
 *   MTP Backend
 *
 * Copyright (C) 2012 Philip Langdale <philipl@overt.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <libmtp.h>

#include "gvfsbackendmtp.h"
#include "gvfsicon.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobdelete.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsjobmakedirectory.h"
#include "gvfsmonitor.h"


/* ------------------------------------------------------------------------------------------------- */

/* showing debug traces */
#define DEBUG_SHOW_TRACES 1
#define DEBUG_SHOW_ENUMERATE_TRACES 0

static void
DEBUG (const gchar *message, ...)
{
#if DEBUG_SHOW_TRACES
  va_list args;
  va_start (args, message);
  g_vfprintf (stderr, message, args);
  va_end (args);
  g_fprintf (stderr, "\n");
  fflush (stderr);
#endif
}

static void
DEBUG_ENUMERATE (const gchar *message, ...)
{
#if DEBUG_SHOW_ENUMERATE_TRACES
  va_list args;
  va_start (args, message);
  g_vfprintf (stderr, message, args);
  va_end (args);
  g_fprintf (stderr, "\n");
  fflush (stderr);
#endif
}


/************************************************
 * Storage constants copied from ptp.h
 *
 * ptp.h is treated as a private header by libmtp
 ************************************************/

/* PTP Storage Types */

#define PTP_ST_Undefined                        0x0000
#define PTP_ST_FixedROM                         0x0001
#define PTP_ST_RemovableROM                     0x0002
#define PTP_ST_FixedRAM                         0x0003
#define PTP_ST_RemovableRAM                     0x0004


/************************************************
 * Initialization
 ************************************************/

G_DEFINE_TYPE (GVfsBackendMtp, g_vfs_backend_mtp, G_VFS_TYPE_BACKEND)

static void
g_vfs_backend_mtp_init (GVfsBackendMtp *backend)
{
  DEBUG ("(I) g_vfs_backend_mtp_init");
  GMountSpec *mount_spec;

  g_mutex_init (&backend->mutex);
  g_vfs_backend_set_display_name (G_VFS_BACKEND (backend), "mtp");
  g_vfs_backend_set_icon_name (G_VFS_BACKEND (backend), "multimedia-player");

  mount_spec = g_mount_spec_new ("mtp");
  g_vfs_backend_set_mount_spec (G_VFS_BACKEND (backend), mount_spec);
  g_mount_spec_unref (mount_spec);

  backend->monitors = g_hash_table_new (NULL, NULL);

  DEBUG ("(I) g_vfs_backend_mtp_init done.");
}

static void
remove_monitor_weak_ref (gpointer monitor,
                         gpointer unused,
                         gpointer monitors)
{
  g_object_weak_unref (G_OBJECT(monitor), (GWeakNotify)g_hash_table_remove, monitors);
}

static void
g_vfs_backend_mtp_finalize (GObject *object)
{
  GVfsBackendMtp *backend;

  DEBUG ("(I) g_vfs_backend_mtp_finalize");

  backend = G_VFS_BACKEND_MTP (object);

  g_hash_table_foreach (backend->monitors, remove_monitor_weak_ref, backend->monitors);
  g_hash_table_unref (backend->monitors);
  g_mutex_clear (&backend->mutex);

  (*G_OBJECT_CLASS (g_vfs_backend_mtp_parent_class)->finalize) (object);

  DEBUG ("(I) g_vfs_backend_mtp_finalize done.");
}


/************************************************
 * Monitors
 ************************************************/

/**
 * do_create_dir_monitor:
 *
 * Called with backend mutex lock held.
 */
static void
do_create_dir_monitor (GVfsBackend *backend,
                       GVfsJobCreateMonitor *job,
                       const char *filename,
                       GFileMonitorFlags flags)
{
  GVfsBackendMtp *mtp_backend = G_VFS_BACKEND_MTP (backend);

  DEBUG ("(I) create_dir_monitor (%s)", filename);

  GVfsMonitor *vfs_monitor = g_vfs_monitor_new (backend);

  g_object_set_data_full (G_OBJECT (vfs_monitor), "gvfsbackendmtp:path",
                          g_strdup (filename), g_free);

  g_vfs_job_create_monitor_set_monitor (job, vfs_monitor);
  g_hash_table_add (mtp_backend->monitors, vfs_monitor);
  g_object_weak_ref (G_OBJECT (vfs_monitor), (GWeakNotify)g_hash_table_remove, mtp_backend->monitors);
  g_object_unref (vfs_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  DEBUG ("(I) create_dir_monitor done.");
}


/**
 * do_create_file_monitor:
 *
 * Called with backend mutex lock held.
 */
static void
do_create_file_monitor (GVfsBackend *backend,
                        GVfsJobCreateMonitor *job,
                        const char *filename,
                        GFileMonitorFlags flags)
{
  GVfsBackendMtp *mtp_backend = G_VFS_BACKEND_MTP (backend);

  DEBUG ("(I) create_file_monitor (%s)", filename);

  GVfsMonitor *vfs_monitor = g_vfs_monitor_new (backend);

  g_object_set_data_full (G_OBJECT (vfs_monitor), "gvfsbackendmtp:path",
                          g_strdup (filename), g_free);

  g_vfs_job_create_monitor_set_monitor (job, vfs_monitor);
  g_hash_table_add (mtp_backend->monitors, vfs_monitor);
  g_object_weak_ref (G_OBJECT (vfs_monitor), (GWeakNotify)g_hash_table_remove, mtp_backend->monitors);
  g_object_unref (vfs_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  DEBUG ("(I) create_file_monitor done.");
}


static void
emit_event_internal (GVfsMonitor *monitor,
                     const char *path,
                     GFileMonitorEvent event)
{
  DEBUG ("(III) emit_event_internal (%s, %d)", path, event);

  char *dir = g_dirname (path);
  const char *monitored_path = g_object_get_data (G_OBJECT (monitor), "gvfsbackendmtp:path");
  if (g_strcmp0 (dir, monitored_path) == 0) {
    DEBUG ("(III) emit_event_internal: Event %d on directory %s for %s", event, dir, path);
    g_vfs_monitor_emit_event (monitor, event, path, NULL);
  } else if (g_strcmp0 (path, monitored_path) == 0) {
    DEBUG ("(III) emit_event_internal: Event %d on file %s", event, path);
    g_vfs_monitor_emit_event (monitor, event, path, NULL);
  }
  g_free (dir);

  DEBUG ("(III) emit_event_internal done.");
}


static void
emit_create_event (gpointer key,
                   gpointer value,
                   gpointer user_data)
{
  DEBUG ("(II) emit_create_event.");
  emit_event_internal (key, user_data, G_FILE_MONITOR_EVENT_CREATED);
}


static void
emit_delete_event (gpointer key,
                   gpointer value,
                   gpointer user_data)
{
  DEBUG ("(II) emit_delete_event.");
  emit_event_internal (key, user_data, G_FILE_MONITOR_EVENT_DELETED);
}


static void
emit_change_event (gpointer key,
                   gpointer value,
                   gpointer user_data)
{
  DEBUG ("(II) emit_change_event.");
  emit_event_internal (key, user_data, G_FILE_MONITOR_EVENT_CHANGED);
}


/************************************************
 * Errors
 ************************************************/

static void
fail_job (GVfsJob *job, LIBMTP_mtpdevice_t *device)
{
  LIBMTP_error_t *error = LIBMTP_Get_Errorstack (device);

  g_vfs_job_failed (job, G_IO_ERROR,
                    g_vfs_job_is_cancelled (job) ?
                      G_IO_ERROR_CANCELLED :
                      G_IO_ERROR_FAILED,
                    _("libmtp error: %s"),
                    g_strrstr (error->error_text, ":") + 1);

  LIBMTP_Clear_Errorstack (device);
}


/************************************************
 * Mounts
 ************************************************/

static LIBMTP_mtpdevice_t *
get_device (GVfsBackend *backend, const char *id, GVfsJob *job);


static void
on_uevent (GUdevClient *client, gchar *action, GUdevDevice *device, gpointer user_data)
{
  const char *dev_path = g_udev_device_get_device_file (device);
  DEBUG ("(I) on_uevent (action %s, device %s)", action, dev_path);

  if (dev_path == NULL) {
    return;
  }

  GVfsBackendMtp *op_backend = G_VFS_BACKEND_MTP (user_data);

  if (g_strcmp0 (op_backend->dev_path, dev_path) == 0 &&
      g_str_equal (action, "remove")) {
    DEBUG ("(I) on_uevent: Quiting after remove event on device %s", dev_path);
    /* TODO: need a cleaner way to force unmount ourselves */
    exit (1);
  }

  DEBUG ("(I) on_uevent done.");
}

#if HAVE_LIBMTP_READ_EVENT
static gpointer
check_event (gpointer user_data)
{
  GWeakRef *event_ref = user_data;

  LIBMTP_event_t event;
  int ret = 0;
  while (ret == 0) {
    uint32_t param1;
    char *path;
    GVfsBackendMtp *backend;

    backend = g_weak_ref_get (event_ref);
    if (backend && !g_atomic_int_get (&backend->unmount_started)) {
      LIBMTP_mtpdevice_t *device = backend->device;
      g_object_unref (backend);
      /*
       * Unavoidable race. We can't hold a reference when
       * calling Read_Event as it blocks while waiting and
       * we can't interrupt it in any sane way, so it would
       * end up preventing finalization of the backend.
       */
      ret = LIBMTP_Read_Event (device, &event, &param1);
    } else {
      return NULL;
    }

    switch (event) {
    case LIBMTP_EVENT_STORE_ADDED:
      backend = g_weak_ref_get (event_ref);
      if (backend && !g_atomic_int_get (&backend->unmount_started)) {
        path = g_strdup_printf ("/%u", param1);
        g_mutex_lock (&backend->mutex);
        g_hash_table_foreach (backend->monitors, emit_create_event, path);
        g_mutex_unlock (&backend->mutex);
        g_free (path);
        g_object_unref (backend);
        break;
      } else {
        return NULL;
      }
    default:
      break;
    }
  }
  return NULL;
}
#endif

static gboolean
mtp_heartbeat (GVfsBackendMtp *backend)
{
  if (g_mutex_trylock (&backend->mutex)) {
    LIBMTP_Dump_Device_Info(backend->device);
    g_mutex_unlock (&backend->mutex);
  }
  return TRUE;
}

static char *
get_dev_path_from_host (GVfsJob *job,
                        GUdevClient *gudev_client,
                        const char *host)
{
  /* turn usb:001,041 string into an udev device name */
  if (!g_str_has_prefix (host, "[usb:")) {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_NOT_SUPPORTED,
                              _("Unexpected host uri format."));
    return NULL;
  }

  char *comma;
  char *dev_path = g_strconcat ("/dev/bus/usb/", host + 5, NULL);
  if ((comma = strchr (dev_path, ',')) == NULL) {
    g_free (dev_path);
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_NOT_SUPPORTED,
                              _("Malformed host uri."));
    return NULL;
  }
  *comma = '/';
  dev_path[strlen (dev_path) -1] = '\0';
  DEBUG ("(II) get_dev_path_from_host: Parsed '%s' into device name %s", host, dev_path);

  /* find corresponding GUdevDevice */
  GUdevDevice *device = g_udev_client_query_by_device_file (gudev_client, dev_path);
  if (!device) {
    g_free (dev_path);
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("Couldn't find matching udev device."));
    return NULL;
  }
  g_object_unref (device);

  return dev_path;
}

static void
do_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendMtp *op_backend = G_VFS_BACKEND_MTP (backend);

  DEBUG ("(I) do_mount");

  const char *host = g_mount_spec_get (mount_spec, "host");
  DEBUG ("(I) do_mount: host=%s", host);
  if (host == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("No device specified"));
    return;
  }

  const char *subsystems[] = {"usb", NULL};
  op_backend->gudev_client = g_udev_client_new (subsystems);
  if (op_backend->gudev_client == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Cannot create gudev client"));
    return;
  }

  char *dev_path = get_dev_path_from_host (G_VFS_JOB (job), op_backend->gudev_client, host);
  if (dev_path == NULL) {
    g_object_unref (op_backend->gudev_client);
    /* get_dev_path_from_host() sets job state. */
    return;
  }
  op_backend->dev_path = dev_path;

  op_backend->on_uevent_id =
    g_signal_connect_object (op_backend->gudev_client, "uevent",
                             G_CALLBACK (on_uevent), op_backend, 0);

  LIBMTP_Init ();

  get_device (backend, host, G_VFS_JOB (job));
  if (!G_VFS_JOB (job)->failed) {
    GMountSpec *mtp_mount_spec = g_mount_spec_new ("mtp");
    g_mount_spec_set (mtp_mount_spec, "host", host);
    g_vfs_backend_set_mount_spec (backend, mtp_mount_spec);
    g_mount_spec_unref (mtp_mount_spec);

    g_vfs_job_succeeded (G_VFS_JOB (job));

    op_backend->hb_id =
      g_timeout_add_seconds (900, (GSourceFunc)mtp_heartbeat, op_backend);

#if HAVE_LIBMTP_READ_EVENT
    GWeakRef *event_ref = g_new0 (GWeakRef, 1);
    g_weak_ref_init (event_ref, backend);
    GThread *event_thread = g_thread_new ("events", check_event, event_ref);
    /*
     * We don't need our ref to the thread, as the libmtp semantics mean
     * that in the normal case, the thread will block forever when we are
     * cleanining up before termination, so we can never join the thread.
     */
    g_thread_unref (event_thread);
#endif
  }
  DEBUG ("(I) do_mount done.");
}


static void
do_unmount (GVfsBackend *backend, GVfsJobUnmount *job,
            GMountUnmountFlags flags,
            GMountSource *mount_source)
{
  GVfsBackendMtp *op_backend;

  DEBUG ("(I) do_umount");

  op_backend = G_VFS_BACKEND_MTP (backend);

  g_mutex_lock (&op_backend->mutex);

  g_atomic_int_set (&op_backend->unmount_started, TRUE);

  g_source_remove (op_backend->hb_id);
  g_signal_handler_disconnect (op_backend->gudev_client,
                               op_backend->on_uevent_id);
  g_object_unref (op_backend->gudev_client);
  g_clear_pointer (&op_backend->dev_path, g_free);
  LIBMTP_Release_Device (op_backend->device);

  g_mutex_unlock (&op_backend->mutex);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  DEBUG ("(I) do_umount done.");
}


/************************************************
 * 	  Queries
 * 
 */


/**
 * get_device:
 *
 * Called with backend mutex lock held.
 */
LIBMTP_mtpdevice_t *
get_device (GVfsBackend *backend, const char *id, GVfsJob *job) {
  DEBUG ("(II) get_device: %s", id);

  LIBMTP_mtpdevice_t *device = NULL;

  if (G_VFS_BACKEND_MTP (backend)->device != NULL) {
    DEBUG ("(II) get_device: Returning cached device %p", device);
    return G_VFS_BACKEND_MTP (backend)->device;
  }

  LIBMTP_raw_device_t * rawdevices;
  int numrawdevices;
  LIBMTP_error_number_t err;

  err = LIBMTP_Detect_Raw_Devices (&rawdevices, &numrawdevices);
  switch (err) {
  case LIBMTP_ERROR_NONE:
    break;
  case LIBMTP_ERROR_NO_DEVICE_ATTACHED:
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("No MTP devices found"));
    goto exit;
  case LIBMTP_ERROR_CONNECTING:
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                              _("Unable to connect to MTP device"));
    goto exit;
  case LIBMTP_ERROR_MEMORY_ALLOCATION:
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                              _("Unable to allocate memory while detecting MTP devices"));
    goto exit;
  case LIBMTP_ERROR_GENERAL:
  default:
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_FAILED,
                              _("Generic libmtp error"));
    goto exit;
  }

  /* Iterate over connected MTP devices */
  int i;
  for (i = 0; i < numrawdevices; i++) {
    char *name;
    name = g_strdup_printf ("[usb:%03u,%03u]",
                            rawdevices[i].bus_location,
                            rawdevices[i].devnum);

    if (strcmp (id, name) == 0) {
      device = LIBMTP_Open_Raw_Device_Uncached (&rawdevices[i]);
      if (device == NULL) {
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Unable to open MTP device '%s'"), name);
        g_free (name);
        goto exit;
      }

      DEBUG ("(II) get_device: Storing device %s", name);
      G_VFS_BACKEND_MTP (backend)->device = device;

      LIBMTP_Dump_Errorstack (device);
      LIBMTP_Clear_Errorstack (device);
      g_free (name);
      break;
    } else {
      g_free (name);
    }
  }

 exit:
  DEBUG ("(II) get_device done.");
  return device;
}


/**
 * get_device_info:
 *
 * Called with backend mutex lock held.
 */
static void
get_device_info (GVfsBackendMtp *backend, GFileInfo *info)
{
  LIBMTP_mtpdevice_t *device = backend->device;
  const char *name;

  name = g_mount_spec_get (g_vfs_backend_get_mount_spec (G_VFS_BACKEND (backend)), "host");

  DEBUG_ENUMERATE ("(II) get_device_info: %s", name);

  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
  g_file_info_set_name (info, name);

  char *friendlyname = LIBMTP_Get_Friendlyname (device);
  g_file_info_set_display_name (info, friendlyname == NULL ?
                                      _("Unnamed Device") : friendlyname);
  free (friendlyname);

  g_file_info_set_content_type (info, "inode/directory");
  g_file_info_set_size (info, 0);

  GIcon *icon = g_themed_icon_new ("multimedia-player");
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, TRUE); 

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "mtpfs");

  int ret = LIBMTP_Get_Storage (device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
  if (ret != 0) {
    LIBMTP_Dump_Errorstack (device);
    LIBMTP_Clear_Errorstack (device);
    DEBUG_ENUMERATE ("(II) get_device_info done with no stores.");
    return;
  }
  guint64 freeSpace = 0;
  guint64 maxSpace = 0;
  LIBMTP_devicestorage_t *storage;
  for (storage = device->storage; storage != 0; storage = storage->next) {
    freeSpace += storage->FreeSpaceInBytes;
    maxSpace += storage->MaxCapacity;
  }

  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, freeSpace);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, maxSpace);

  DEBUG_ENUMERATE ("(II) get_device_info done.");
}


/**
 * get_storage_info:
 *
 * Called with backend mutex lock held.
 */
static void
get_storage_info (LIBMTP_devicestorage_t *storage, GFileInfo *info) {

  char *id = g_strdup_printf ("%u", storage->id);
  g_file_info_set_name (info, id);
  g_free (id);

  DEBUG_ENUMERATE ("(II) get_storage_info: %s", storage->id);

  g_file_info_set_display_name (info, storage->StorageDescription);
  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
  g_file_info_set_content_type (info, "inode/directory");
  g_file_info_set_size (info, 0);

  GIcon *icon;
  switch (storage->StorageType) {
  case PTP_ST_FixedROM:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
    icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk");
    break;
  case PTP_ST_RemovableROM:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
    icon = g_themed_icon_new_with_default_fallbacks ("media-memory-sd");
    break;
  case PTP_ST_RemovableRAM:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, FALSE);
    icon = g_themed_icon_new_with_default_fallbacks ("media-memory-sd");
    break;
  case PTP_ST_FixedRAM:
  default:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, FALSE);
    icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk");
    break;
  }
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE); 

  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, storage->FreeSpaceInBytes);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, storage->MaxCapacity);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "mtpfs");

  DEBUG_ENUMERATE ("(II) get_storage_info done.");
}


/**
 * get_file_info:
 *
 * Called with backend mutex lock held.
 */
static void
get_file_info (GVfsBackend *backend,
               LIBMTP_mtpdevice_t *device,
               GFileInfo *info,
               LIBMTP_file_t *file) {
  GIcon *icon = NULL;
  char *content_type = NULL;

  char *id = g_strdup_printf ("%u", file->item_id);
  g_file_info_set_name (info, id);
  g_free (id);

  DEBUG_ENUMERATE ("(II) get_file_info: %u", file->item_id);

  g_file_info_set_display_name (info, file->filename);

  switch (file->filetype) {
  case LIBMTP_FILETYPE_FOLDER:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_content_type (info, "inode/directory");
    icon = g_themed_icon_new ("folder");
    break;
  default:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, FALSE);
    g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);
    content_type = g_content_type_guess (file->filename, NULL, 0, NULL);
    g_file_info_set_content_type (info, content_type);
    icon = g_content_type_get_icon (content_type);
    break;
  }


#if HAVE_LIBMTP_GET_THUMBNAIL
  if (LIBMTP_FILETYPE_IS_IMAGE (file->filetype) ||
      LIBMTP_FILETYPE_IS_VIDEO (file->filetype) ||
      LIBMTP_FILETYPE_IS_AUDIOVIDEO (file->filetype)) {

    GIcon *preview;
    char *icon_id;
    GMountSpec *mount_spec;

    mount_spec = g_vfs_backend_get_mount_spec (backend);
    icon_id = g_strdup_printf ("%u", file->item_id);
    preview = g_vfs_icon_new (mount_spec,
                              icon_id);
    g_file_info_set_attribute_object (info,
                                      G_FILE_ATTRIBUTE_PREVIEW_ICON,
                                      G_OBJECT (preview));
    g_object_unref (preview);
    g_free (icon_id);
  }
#endif

  g_file_info_set_size (info, file->filesize);

  GTimeVal modtime = { file->modificationdate, 0 };
  g_file_info_set_modification_time (info, &modtime);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, TRUE);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_COPY_NAME, file->filename);


  if (icon != NULL) {
    g_file_info_set_icon (info, icon);
    g_object_unref (icon);
  }
  g_free (content_type);

  DEBUG_ENUMERATE ("(II) get_file_info done.");
}


static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *job,
              const char *filename,
              GFileAttributeMatcher *attribute_matcher,
              GFileQueryInfoFlags flags)
{
  GVfsBackendMtp *op_backend = G_VFS_BACKEND_MTP (backend);
  GFileInfo *info;

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  DEBUG ("(I) do_enumerate (filename = %s, n_elements = %d) ", filename, ne);

  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  LIBMTP_mtpdevice_t *device;
  device = op_backend->device;

  if (ne == 2 && elements[1][0] == '\0') {
    LIBMTP_devicestorage_t *storage;

    int ret = LIBMTP_Get_Storage (device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      LIBMTP_Dump_Errorstack (device);
      LIBMTP_Clear_Errorstack (device);
      goto success;
    }
    for (storage = device->storage; storage != 0; storage = storage->next) {
      info = g_file_info_new ();
      get_storage_info (storage, info);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref (info);
    }
  } else {
    LIBMTP_file_t *files;

    int pid = (ne == 2 ? -1 : strtol (elements[ne-1], NULL, 10));

    LIBMTP_Clear_Errorstack (device);
    files = LIBMTP_Get_Files_And_Folders (device, strtol (elements[1], NULL, 10), pid);
    if (files == NULL && LIBMTP_Get_Errorstack (device) != NULL) {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }
    while (files != NULL) {
      LIBMTP_file_t *file = files;
      files = files->next;

      info = g_file_info_new ();
      get_file_info (backend, device, info, file);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref (info);

      LIBMTP_destroy_file_t (file);
    }
  }

 success:
  g_vfs_job_enumerate_done (job);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
  DEBUG ("(I) do_enumerate done.");
}


/**
 * get_file_for_filename:
 *
 * Get the entity ID for an element given its filename and
 * the IDs of its parents.
 *
 * Called with backend mutex lock held.
 */
static LIBMTP_file_t *
get_file_for_filename (LIBMTP_mtpdevice_t *device,
                       gchar **elements,
                       unsigned int i)
{
  LIBMTP_file_t *file = NULL;

  DEBUG ("(III) get_file_for_filename (element %d '%s') ", i, elements[i]);
  long parent_id = -1;
  if (i > 2) {
    parent_id = strtol (elements[i - 1], NULL, 10);
  }
  LIBMTP_file_t *f = LIBMTP_Get_Files_And_Folders (device, strtol (elements[1], NULL, 10),
                                                   parent_id);
  while (f != NULL) {
    DEBUG_ENUMERATE ("(III) query (entity = %s, name = %s) ", f->filename, elements[i]);
    if (strcmp (f->filename, elements[i]) == 0) {
      file = f;
      f = f->next;
      break;
    } else {
      LIBMTP_file_t *tmp = f;
      f = f->next;
      LIBMTP_destroy_file_t (tmp);
    }
  }
  while (f != NULL) {
    LIBMTP_file_t *tmp = f;
    f = f->next;
    LIBMTP_destroy_file_t (tmp);
  }
  DEBUG ("(III) get_file_for_filename done");
  return file;
}


/**
 * normalize_elements:
 *
 * Take a set of path elements and turn any file/directory names into
 * MTP entity IDs.
 *
 * Called with backend mutex lock held.
 */
static void
normalize_elements (LIBMTP_mtpdevice_t *device,
                    gchar **elements,
                    unsigned int ne)
{
  DEBUG ("(II) normalize_elements (ne = %d)", ne);
  if (ne < 3) {
    /* In these cases, elements are always normal. */
    return;
  }

  unsigned int i;
  for (i = 2; i < ne; i++) {
    LIBMTP_file_t *file = NULL;
    char *endptr;
    long file_id = strtol (elements[i], &endptr, 10);

    if (file_id == 0 || *endptr != '\0') {
      file = get_file_for_filename(device, elements, i);
      if (file == NULL) {
        /* Missing entity. Cannot normalize. */
        DEBUG ("(II) Cannot normalize missing entity '%s'", elements[i]);
        continue;
      } else {
        char *item_id = g_strdup_printf ("%d", file->item_id);
        DEBUG ("(II) %s = %s", elements[i], item_id);
        g_free (elements[i]);
        elements[i] = item_id;
        LIBMTP_destroy_file_t (file);
      }
    } else {
      /* Already normal. */
      DEBUG ("(II) normal entity '%s'", elements[i]);
      continue;
    }
  }
  DEBUG ("(II) normalize_elements done");
}

static void
do_query_info (GVfsBackend *backend,
               GVfsJobQueryInfo *job,
               const char *filename,
               GFileQueryInfoFlags flags,
               GFileInfo *info,
               GFileAttributeMatcher *matcher)
{
  DEBUG ("(I) do_query_info (filename = %s) ", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  if (ne == 2 && elements[1][0] == '\0') {
    get_device_info (G_VFS_BACKEND_MTP (backend), info);
  } else if (ne < 3) {
    LIBMTP_devicestorage_t *storage;
    int ret = LIBMTP_Get_Storage (device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      LIBMTP_Dump_Errorstack (device);
      LIBMTP_Clear_Errorstack (device);
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("No storage volumes found"));
      goto exit;
    }
    for (storage = device->storage; storage != 0; storage = storage->next) {
      if (storage->id == strtol (elements[ne-1], NULL, 10)) {
        DEBUG ("(I) found storage %u", storage->id);
        get_storage_info (storage, info);
      }
    }
  } else {
    LIBMTP_file_t *file = NULL;
    char *endptr;
    long file_id = strtol (elements[ne - 1], &endptr, 10);

    if (file_id == 0 || *endptr != '\0') {
      file = get_file_for_filename (device, elements, ne - 1);
      if (file == NULL) {
        /* The backup query might have found nothing. */
        DEBUG ("(I) get_file_for_filename could not find '%s'",
               elements[ne - 1]);
        g_vfs_job_failed_literal (G_VFS_JOB (job),
                                  G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                  _("File not found"));
        goto exit;
      }
    } else {
      file = LIBMTP_Get_Filemetadata (device, file_id);
    }

    if (file != NULL) {
      get_file_info (backend, device, info, file);
      LIBMTP_destroy_file_t (file);
    } else {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
  DEBUG ("(I) do_query_info done.");
}


static void
do_query_fs_info (GVfsBackend *backend,
		  GVfsJobQueryFsInfo *job,
		  const char *filename,
		  GFileInfo *info,
		  GFileAttributeMatcher *attribute_matcher)
{
  DEBUG ("(I) do_query_fs_info (filename = %s) ", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  if (ne == 2 && elements[1][0] == '\0') {
    get_device_info (G_VFS_BACKEND_MTP (backend), info);
  } else {
    LIBMTP_devicestorage_t *storage;
    int ret = LIBMTP_Get_Storage (device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      LIBMTP_Dump_Errorstack (device);
      LIBMTP_Clear_Errorstack (device);
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("No storage volumes found"));
      goto exit;
    }
    for (storage = device->storage; storage != 0; storage = storage->next) {
      if (storage->id == strtol (elements[1], NULL, 10)) {
        get_storage_info (storage, info);
      }
    }
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  DEBUG ("(I) do_query_fs_info done.");
}


/************************************************
 * 	  Operations
 * 
 */

typedef struct {
  GFileProgressCallback progress_callback;
  gpointer progress_callback_data;
  GVfsJob *job;
} MtpProgressData;


static int
mtp_progress (uint64_t const sent, uint64_t const total,
              MtpProgressData const * const data)
{
  if (data->progress_callback) {
    data->progress_callback (sent, total, data->progress_callback_data);
  }
  return g_vfs_job_is_cancelled (data->job);
}


static void
do_make_directory (GVfsBackend *backend,
                   GVfsJobMakeDirectory *job,
                   const char *filename)
{
  DEBUG ("(I) do_make_directory (filename = %s) ", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  if (ne < 3) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_FAILED,
                              _("Cannot make directory in this location"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  /*
   * Might be called as part of a batch copy of a nested directory hierarchy.
   * New directories would then be referred to by name and not id.
   */
  normalize_elements(device, elements, ne - 1);

  int parent_id = 0;
  if (ne > 3) {
    parent_id = strtol (elements[ne-2], NULL, 10);
  }

  int ret = LIBMTP_Create_Folder (device, elements[ne-1], parent_id, strtol (elements[1], NULL, 10));
  if (ret == 0) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors, emit_create_event, (char *)filename);

 exit:
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  DEBUG ("(I) do_make_directory done.");
}


static void
do_pull (GVfsBackend *backend,
         GVfsJobPull *job,
         const char *source,
         const char *local_path,
         GFileCopyFlags flags,
         gboolean remove_source,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  DEBUG ("(I) do_pull (filename = %s, local_path = %s) ", source, local_path);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  GFileInfo *info = NULL;
  gchar **elements = g_strsplit_set (source, "/", -1);
  unsigned int ne = g_strv_length (elements);

  if (ne < 3) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                              _("Not a regular file"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, strtol (elements[ne-1], NULL, 10));
  if (file == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File does not exist"));
    goto exit;
  }

  info = g_file_info_new ();
  get_file_info (backend, device, info, file);
  LIBMTP_destroy_file_t (file);
  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                              _("Can't recursively copy directory"));
    goto exit;
  } else {
      MtpProgressData mtp_progress_data;
      mtp_progress_data.progress_callback = progress_callback;
      mtp_progress_data.progress_callback_data = progress_callback_data;
      mtp_progress_data.job = G_VFS_JOB (job);
      int ret = LIBMTP_Get_File_To_File (device,
                                         strtol (elements[ne-1], NULL, 10),
                                         local_path,
                                         (LIBMTP_progressfunc_t)mtp_progress,
                                         &mtp_progress_data);
      if (ret != 0) {
        fail_job (G_VFS_JOB (job), device);
        goto exit;
      }
    g_vfs_job_succeeded (G_VFS_JOB (job));
  }

 exit:
  g_clear_object (&info);
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  DEBUG ("(I) do_pull done.");
}


static void
do_push (GVfsBackend *backend,
         GVfsJobPush *job,
         const char *destination,
         const char *local_path,
         GFileCopyFlags flags,
         gboolean remove_source,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  DEBUG ("(I) do_push (filename = %s, local_path = %s) ", destination, local_path);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  GFile *file = NULL;
  GFileInfo *info = NULL;
  gchar **elements = g_strsplit_set (destination, "/", -1);
  unsigned int ne = g_strv_length (elements);

  if (ne < 3) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                              _("Cannot write to this location"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  /*
   * Might be called as part of a batch copy of a nested directory hierarchy.
   * New files would then be referred to by name and not id.
   */
  normalize_elements(device, elements, ne - 1);

  int parent_id = 0;

  if (ne > 3) {
    parent_id = strtol (elements[ne-2], NULL, 10);
  }

  file = g_file_new_for_path (local_path);
  g_assert(file);

  if (g_file_query_file_type (file, G_FILE_QUERY_INFO_NONE,
                              G_VFS_JOB (job)->cancellable) ==
      G_FILE_TYPE_DIRECTORY) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                              _("Can't recursively copy directory"));
    goto exit;
  }

  GError *error = NULL;
  info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                            G_FILE_QUERY_INFO_NONE,
                            G_VFS_JOB (job)->cancellable,
                            &error);
  if (!info) {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
    g_error_free (error);
    goto exit;
  }

  LIBMTP_file_t *mtpfile = LIBMTP_new_file_t ();
  mtpfile->filename = strdup (elements[ne-1]);
  mtpfile->parent_id = parent_id;
  mtpfile->storage_id = strtol (elements[1], NULL, 10);
  mtpfile->filetype = LIBMTP_FILETYPE_UNKNOWN; 
  mtpfile->filesize = g_file_info_get_size (info);

  MtpProgressData mtp_progress_data;
  mtp_progress_data.progress_callback = progress_callback;
  mtp_progress_data.progress_callback_data = progress_callback_data;
  mtp_progress_data.job = G_VFS_JOB (job);
  int ret = LIBMTP_Send_File_From_File (device, local_path, mtpfile,
                                        (LIBMTP_progressfunc_t)mtp_progress,
                                        &mtp_progress_data);
  LIBMTP_destroy_file_t (mtpfile);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_create_event,
                        (char *)destination);

 exit:
  g_clear_object (&file);
  g_clear_object (&info);
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  DEBUG ("(I) do_push done.");
}


static void
do_delete (GVfsBackend *backend,
            GVfsJobDelete *job,
            const char *filename)
{
  DEBUG ("(I) do_delete (filename = %s) ", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  if (ne < 3) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_FAILED,
                              _("Cannot delete this entity"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  int ret = LIBMTP_Delete_Object (device, strtol (elements[ne-1], NULL, 10));
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }
  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_delete_event,
                        (char *)filename);

 exit:
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  DEBUG ("(I) do_delete done.");
}


static void
do_set_display_name (GVfsBackend *backend,
                      GVfsJobSetDisplayName *job,
                      const char *filename,
                      const char *display_name)
{
  DEBUG ("(I) do_set_display_name '%s' --> '%s' ", filename, display_name);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  if (ne < 3) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                              _("Can't rename volume"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, strtol (elements[ne-1], NULL, 10));
  int ret = LIBMTP_Set_File_Name (device, file, display_name);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }
  LIBMTP_destroy_file_t (file);
  file = NULL;
  g_vfs_job_set_display_name_set_new_path (job, filename);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_change_event,
                        (char *)filename);

 exit:
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  DEBUG ("(I) do_set_display_name done.");
}


#if HAVE_LIBMTP_GET_THUMBNAIL
static void
do_open_icon_for_read (GVfsBackend *backend,
                       GVfsJobOpenIconForRead *job,
                       const char *icon_id)
{
  DEBUG ("(I) do_open_icon_for_read (%s)", icon_id);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  guint id = strtol (icon_id, NULL, 10);

  if (id > 0) {
    unsigned char *data;
    unsigned int size;
    int ret = LIBMTP_Get_Thumbnail (G_VFS_BACKEND_MTP (backend)->device, id,
                                    &data, &size);
    if (ret == 0) {
      DEBUG ("File %u has thumbnail: %u", id, size);
      GByteArray *bytes = g_byte_array_sized_new (size);
      g_byte_array_append (bytes, data, size);
      free (data);
      g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ (job), FALSE);
      g_vfs_job_open_for_read_set_handle (G_VFS_JOB_OPEN_FOR_READ (job), bytes);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    } else {
      LIBMTP_filesampledata_t *sample_data = LIBMTP_new_filesampledata_t ();
      ret = LIBMTP_Get_Representative_Sample (G_VFS_BACKEND_MTP (backend)->device,
                                              id, sample_data);
      if (ret == 0) {
        DEBUG ("File %u has sampledata: %u", id, size);
        GByteArray *bytes = g_byte_array_sized_new (sample_data->size);
        g_byte_array_append (bytes, (const guint8 *)sample_data->data, sample_data->size);
        LIBMTP_destroy_filesampledata_t (sample_data);
        g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ (job), FALSE);
        g_vfs_job_open_for_read_set_handle (G_VFS_JOB_OPEN_FOR_READ (job), bytes);
        g_vfs_job_succeeded (G_VFS_JOB (job));
      } else {
        DEBUG ("File %u has no thumbnail:", id);
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR,
                          G_IO_ERROR_NOT_FOUND,
                          _("No thumbnail for entity '%s'"),
                          icon_id);
      }
    }
  } else {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR,
                      G_IO_ERROR_INVALID_ARGUMENT,
                      _("Malformed icon identifier '%s'"),
                      icon_id);
  }
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  DEBUG ("(I) do_open_icon_for_read done.");
}


static gboolean
try_read (GVfsBackend *backend,
          GVfsJobRead *job,
          GVfsBackendHandle handle,
          char *buffer,
          gsize bytes_requested)
{
  GByteArray *bytes = handle;

  DEBUG ("(I) try_read (%u %lu)", bytes->len, bytes_requested);

  gsize bytes_to_copy =  MIN (bytes->len, bytes_requested);
  if (bytes_to_copy == 0) {
    goto out;
  }
  memcpy (buffer, bytes->data, bytes_to_copy);
  g_byte_array_remove_range (bytes, 0, bytes_to_copy);

 out:
  g_vfs_job_read_set_size (job, bytes_to_copy);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  DEBUG ("(I) try_read done.");
  return TRUE;
}

static void
do_close_read (GVfsBackend *backend,
                GVfsJobCloseRead *job,
                GVfsBackendHandle handle)
{
  DEBUG ("(I) do_close_read");
  g_byte_array_unref (handle);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  DEBUG ("(I) do_close_read done.");
}
#endif /* HAVE_LIBMTP_GET_THUMBNAIL */


/************************************************
 * 	  Class init
 *
 */


static void
g_vfs_backend_mtp_class_init (GVfsBackendMtpClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  gobject_class->finalize = g_vfs_backend_mtp_finalize;

  backend_class->mount = do_mount;
  backend_class->unmount = do_unmount;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->query_fs_info = do_query_fs_info;
  backend_class->pull = do_pull;
  backend_class->push = do_push;
  backend_class->make_directory = do_make_directory;
  backend_class->delete = do_delete;
  backend_class->set_display_name = do_set_display_name;
  backend_class->create_dir_monitor = do_create_dir_monitor;
  backend_class->create_file_monitor = do_create_file_monitor;
#if HAVE_LIBMTP_GET_THUMBNAIL
  backend_class->open_icon_for_read = do_open_icon_for_read;
  backend_class->try_read = try_read;
  backend_class->close_read = do_close_read;
#endif
}
