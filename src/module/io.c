#include <stdlib.h>
#include <string.h>

#include "uv.h"

#include "scheduler.h"
#include "vm.h"
#include "wren.h"

#include <stdio.h>
#include <fcntl.h>

// Windows doesn't define all of the Unix permission and mode flags by default,
// so map them ourselves.
#if defined(WIN32) || defined(WIN64)
  #include <sys\stat.h>

  // Map to Windows permission flags.
  #define S_IRUSR _S_IREAD
  #define S_IWUSR _S_IWRITE

  #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
  #define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

  // Not supported on Windows.
  #define O_SYNC 0
#endif

typedef struct sFileRequestData
{
  WrenValue* fiber;
  uv_buf_t buffer;
} FileRequestData;

static const int stdinDescriptor = 0;

// Handle to the Stat class object.
static WrenValue* statClass = NULL;

// Handle to the Stdin class object.
static WrenValue* stdinClass = NULL;

// Handle to an onData_() method call. Called when libuv provides data on stdin.
static WrenValue* stdinOnData = NULL;

// The stream used to read from stdin. Initialized on the first read.
static uv_stream_t* stdinStream = NULL;

// Frees all resources related to stdin.
static void shutdownStdin()
{
  if (stdinStream != NULL)
  {
    uv_close((uv_handle_t*)stdinStream, NULL);
    free(stdinStream);
    stdinStream = NULL;
  }
  
  if (stdinClass != NULL)
  {
    wrenReleaseValue(getVM(), stdinClass);
    stdinClass = NULL;
  }
  
  if (stdinOnData != NULL)
  {
    wrenReleaseValue(getVM(), stdinOnData);
    stdinOnData = NULL;
  }
}

void ioShutdown()
{
  shutdownStdin();
  
  if (statClass != NULL)
  {
    wrenReleaseValue(getVM(), statClass);
    statClass = NULL;
  }
}

// If [request] failed with an error, sends the runtime error to the VM and
// frees the request.
//
// Returns true if an error was reported.
static bool handleRequestError(uv_fs_t* request)
{
  if (request->result >= 0) return false;

  FileRequestData* data = (FileRequestData*)request->data;
  WrenValue* fiber = (WrenValue*)data->fiber;
  
  schedulerResumeError(fiber, uv_strerror((int)request->result));
  
  free(data);
  uv_fs_req_cleanup(request);
  free(request);
  return true;
}

// Allocates a new request that resumes [fiber] when it completes.
uv_fs_t* createRequest(WrenValue* fiber)
{
  uv_fs_t* request = (uv_fs_t*)malloc(sizeof(uv_fs_t));
  
  FileRequestData* data = (FileRequestData*)malloc(sizeof(FileRequestData));
  data->fiber = fiber;
  
  request->data = data;
  return request;
}

// Releases resources used by [request].
//
// Returns the fiber that should be resumed after [request] completes.
WrenValue* freeRequest(uv_fs_t* request)
{
  FileRequestData* data = (FileRequestData*)request->data;
  WrenValue* fiber = data->fiber;
  
  free(data);
  uv_fs_req_cleanup(request);
  free(request);
  
  return fiber;
}

static void directoryListCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;

  uv_dirent_t entry;

  WrenVM* vm = getVM();
  wrenEnsureSlots(vm, 3);
  wrenSetSlotNewList(vm, 2);
  
  while (uv_fs_scandir_next(request, &entry) != UV_EOF)
  {
    wrenSetSlotString(vm, 1, entry.name);
    wrenInsertInList(vm, 2, -1, 1);
  }
  
  schedulerResume(freeRequest(request), true);
  schedulerFinishResume();
}

void directoryList(WrenVM* vm)
{
  const char* path = wrenGetSlotString(vm, 1);
  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 2));
  
  // TODO: Check return.
  uv_fs_scandir(getLoop(), request, path, 0, directoryListCallback);
}

void fileAllocate(WrenVM* vm)
{
  // Store the file descriptor in the foreign data, so that we can get to it
  // in the finalizer.
  int* fd = (int*)wrenSetSlotNewForeign(vm, 0, 0, sizeof(int));
  *fd = (int)wrenGetSlotDouble(vm, 1);
}

void fileFinalize(void* data)
{
  int fd = *(int*)data;
  
  // Already closed.
  if (fd == -1) return;
  
  uv_fs_t request;
  uv_fs_close(getLoop(), &request, fd, NULL);
  uv_fs_req_cleanup(&request);
}

static void fileDeleteCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;
  schedulerResume(freeRequest(request), false);
}

void fileDelete(WrenVM* vm)
{
  const char* path = wrenGetSlotString(vm, 1);
  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 2));
  
  // TODO: Check return.
  uv_fs_unlink(getLoop(), request, path, fileDeleteCallback);
}

static void fileOpenCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;
  
  double fd = (double)request->result;
  schedulerResume(freeRequest(request), true);
  wrenSetSlotDouble(getVM(), 2, fd);
  schedulerFinishResume();
}

// The UNIX file flags have specified names but not values. So we define our
// own values in FileFlags and remap them to the host OS's values here.
static int mapFileFlags(int flags)
{
  int result = 0;
  
  // Note: These must be kept in sync with FileFlags in io.wren.
  if (flags & 0x01) result |= O_RDONLY;
  if (flags & 0x02) result |= O_WRONLY;
  if (flags & 0x04) result |= O_RDWR;
  if (flags & 0x08) result |= O_SYNC;
  if (flags & 0x10) result |= O_CREAT;
  if (flags & 0x20) result |= O_TRUNC;
  if (flags & 0x40) result |= O_EXCL;
  
  return result;
}

void fileOpen(WrenVM* vm)
{
  const char* path = wrenGetSlotString(vm, 1);
  int flags = (int)wrenGetSlotDouble(vm, 2);
  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 3));

  // TODO: Allow controlling access.
  uv_fs_open(getLoop(), request, path, mapFileFlags(flags), S_IRUSR | S_IWUSR,
             fileOpenCallback);
}

// Called by libuv when the stat call for size completes.
static void fileSizeCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;

  double size = (double)request->statbuf.st_size;
  schedulerResume(freeRequest(request), true);
  wrenSetSlotDouble(getVM(), 2, size);
  schedulerFinishResume();
}

void fileSizePath(WrenVM* vm)
{
  const char* path = wrenGetSlotString(vm, 1);
  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 2));
  uv_fs_stat(getLoop(), request, path, fileSizeCallback);
}

static void fileCloseCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;

  schedulerResume(freeRequest(request), false);
}

void fileClose(WrenVM* vm)
{
  int* foreign = (int*)wrenGetSlotForeign(vm, 0);
  int fd = *foreign;

  // If it's already closed, we're done.
  if (fd == -1)
  {
    wrenSetSlotBool(vm, 0, true);
    return;
  }

  // Mark it closed immediately.
  *foreign = -1;

  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 1));
  uv_fs_close(getLoop(), request, fd, fileCloseCallback);
  wrenSetSlotBool(vm, 0, false);
}

void fileDescriptor(WrenVM* vm)
{
  int* foreign = (int*)wrenGetSlotForeign(vm, 0);
  int fd = *foreign;
  wrenSetSlotDouble(vm, 0, fd);
}

static void fileReadBytesCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;

  FileRequestData* data = (FileRequestData*)request->data;
  uv_buf_t buffer = data->buffer;
  size_t count = request->result;

  // TODO: Having to copy the bytes here is a drag. It would be good if Wren's
  // embedding API supported a way to *give* it bytes that were previously
  // allocated using Wren's own allocator.
  schedulerResume(freeRequest(request), true);
  wrenSetSlotBytes(getVM(), 2, buffer.base, count);
  schedulerFinishResume();

  // TODO: Likewise, freeing this after we resume is lame.
  free(buffer.base);
}

void fileReadBytes(WrenVM* vm)
{
  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 3));

  int fd = *(int*)wrenGetSlotForeign(vm, 0);
  // TODO: Assert fd != -1.

  FileRequestData* data = (FileRequestData*)request->data;
  size_t length = (size_t)wrenGetSlotDouble(vm, 1);
  size_t offset = (size_t)wrenGetSlotDouble(vm, 2);

  data->buffer.len = length;
  data->buffer.base = (char*)malloc(length);

  uv_fs_read(getLoop(), request, fd, &data->buffer, 1, offset,
             fileReadBytesCallback);
}

static void realPathCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;
  
  wrenEnsureSlots(getVM(), 3);
  wrenSetSlotString(getVM(), 2, (char*)request->ptr);
  schedulerResume(freeRequest(request), true);
  schedulerFinishResume();
}

void fileRealPath(WrenVM* vm)
{
  const char* path = wrenGetSlotString(vm, 1);
  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 2));
  uv_fs_realpath(getLoop(), request, path, realPathCallback);
}

// Called by libuv when the stat call completes.
static void statCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;
  
  WrenVM* vm = getVM();
  wrenEnsureSlots(vm, 3);
  
  // Get a handle to the Stat class. We'll hang on to this so we don't have to
  // look it up by name every time.
  if (statClass == NULL)
  {
    wrenGetVariable(vm, "io", "Stat", 0);
    statClass = wrenGetSlotValue(vm, 0);
  }
  
  // Create a foreign Stat object to store the stat struct.
  wrenSetSlotValue(vm, 2, statClass);
  wrenSetSlotNewForeign(vm, 2, 2, sizeof(uv_stat_t));
  
  // Copy the stat data.
  uv_stat_t* data = (uv_stat_t*)wrenGetSlotForeign(vm, 2);
  *data = request->statbuf;
  
  schedulerResume(freeRequest(request), true);
  schedulerFinishResume();
}

void fileStat(WrenVM* vm)
{
  int fd = *(int*)wrenGetSlotForeign(vm, 0);
  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 1));
  uv_fs_fstat(getLoop(), request, fd, statCallback);
}

void fileSize(WrenVM* vm)
{
  int fd = *(int*)wrenGetSlotForeign(vm, 0);
  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 1));
  uv_fs_fstat(getLoop(), request, fd, fileSizeCallback);
}

static void fileWriteBytesCallback(uv_fs_t* request)
{
  if (handleRequestError(request)) return;
 
  FileRequestData* data = (FileRequestData*)request->data;
  free(data->buffer.base);

  schedulerResume(freeRequest(request), false);
}

void fileWriteBytes(WrenVM* vm)
{
  int fd = *(int*)wrenGetSlotForeign(vm, 0);
  int length;
  const char* bytes = wrenGetSlotBytes(vm, 1, &length);
  size_t offset = (size_t)wrenGetSlotDouble(vm, 2);
  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 3));
  
  FileRequestData* data = (FileRequestData*)request->data;

  data->buffer.len = length;
  // TODO: Instead of copying, just create a WrenValue for the byte string and
  // hold on to it in the request until the write is done.
  // TODO: Handle allocation failure.
  data->buffer.base = (char*)malloc(length);
  memcpy(data->buffer.base, bytes, length);

  uv_fs_write(getLoop(), request, fd, &data->buffer, 1, offset,
              fileWriteBytesCallback);
}

void statPath(WrenVM* vm)
{
  const char* path = wrenGetSlotString(vm, 1);
  uv_fs_t* request = createRequest(wrenGetSlotValue(vm, 2));
  uv_fs_stat(getLoop(), request, path, statCallback);
}

void statBlockCount(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, (double)stat->st_blocks);
}

void statBlockSize(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, (double)stat->st_blksize);
}

void statDevice(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, (double)stat->st_dev);
}

void statGroup(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, (double)stat->st_gid);
}

void statInode(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, (double)stat->st_ino);
}

void statLinkCount(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, (double)stat->st_nlink);
}

void statMode(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, (double)stat->st_mode);
}

void statSize(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, (double)stat->st_size);
}

void statSpecialDevice(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, (double)stat->st_rdev);
}

void statUser(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotDouble(vm, 0, (double)stat->st_uid);
}

void statIsDirectory(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotBool(vm, 0, S_ISDIR(stat->st_mode));
}

void statIsFile(WrenVM* vm)
{
  uv_stat_t* stat = (uv_stat_t*)wrenGetSlotForeign(vm, 0);
  wrenSetSlotBool(vm, 0, S_ISREG(stat->st_mode));
}

static void allocCallback(uv_handle_t* handle, size_t suggestedSize,
                          uv_buf_t* buf)
{
  // TODO: Handle allocation failure.
  buf->base = (char*)malloc(suggestedSize);
  buf->len = suggestedSize;
}

static void stdinReadCallback(uv_stream_t* stream, ssize_t numRead,
                               const uv_buf_t* buffer)
{
  WrenVM* vm = getVM();
  
  if (stdinClass == NULL)
  {
    wrenEnsureSlots(vm, 1);
    wrenGetVariable(vm, "io", "Stdin", 0);
    stdinClass = wrenGetSlotValue(vm, 0);
  }
  
  if (stdinOnData == NULL)
  {
    stdinOnData = wrenMakeCallHandle(vm, "onData_(_)");
  }
  
  // If stdin was closed, send null to let io.wren know.
  if (numRead == UV_EOF)
  {
    wrenEnsureSlots(vm, 2);
    wrenSetSlotValue(vm, 0, stdinClass);
    wrenSetSlotNull(vm, 1);
    wrenCall(vm, stdinOnData);
    
    shutdownStdin();
    return;
  }

  // TODO: Handle other errors.

  // TODO: Having to copy the bytes here is a drag. It would be good if Wren's
  // embedding API supported a way to *give* it bytes that were previously
  // allocated using Wren's own allocator.
  wrenEnsureSlots(vm, 2);
  wrenSetSlotValue(vm, 0, stdinClass);
  wrenSetSlotBytes(vm, 1, buffer->base, numRead);
  wrenCall(vm, stdinOnData);

  // TODO: Likewise, freeing this after we resume is lame.
  free(buffer->base);
}

void stdinReadStart(WrenVM* vm)
{
  if (stdinStream == NULL)
  {
    if (uv_guess_handle(stdinDescriptor) == UV_TTY)
    {
      // stdin is connected to a terminal.
      uv_tty_t* handle = (uv_tty_t*)malloc(sizeof(uv_tty_t));
      uv_tty_init(getLoop(), handle, stdinDescriptor, true);
      stdinStream = (uv_stream_t*)handle;
    }
    else
    {
      // stdin is a pipe or a file.
      uv_pipe_t* handle = (uv_pipe_t*)malloc(sizeof(uv_pipe_t));
      uv_pipe_init(getLoop(), handle, false);
      uv_pipe_open(handle, stdinDescriptor);
      stdinStream = (uv_stream_t*)handle;
    }
  }

  uv_read_start(stdinStream, allocCallback, stdinReadCallback);
  // TODO: Check return.
}

void stdinReadStop(WrenVM* vm)
{
  uv_read_stop(stdinStream);
}
