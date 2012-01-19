/* OpenCL runtime library: clEnqueueNDRangeKernel()

   Copyright (c) 2011 Universidad Rey Juan Carlos
   
   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:
   
   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.
   
   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#include "pocl_cl.h"
#include "utlist.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

#define COMMAND_LENGTH 1024
#define ARGUMENT_STRING_LENGTH 32

CL_API_ENTRY cl_int CL_API_CALL
clEnqueueNDRangeKernel(cl_command_queue command_queue,
                       cl_kernel kernel,
                       cl_uint work_dim,
                       const size_t *global_work_offset,
                       const size_t *global_work_size,
                       const size_t *local_work_size,
                       cl_uint num_events_in_wait_list,
                       const cl_event *event_wait_list,
                       cl_event *event) CL_API_SUFFIX__VERSION_1_0
{
  char template[] = ".clekXXXXXX";
  size_t offset_x, offset_y, offset_z;
  size_t global_x, global_y, global_z;
  size_t local_x, local_y, local_z;
  char *tmpdir;
  char kernel_filename[POCL_FILENAME_LENGTH];
  FILE *kernel_file;
  char parallel_filename[POCL_FILENAME_LENGTH];
  size_t n;
  struct stat buf;
  char command[COMMAND_LENGTH];
  int error;
  struct pocl_context pc;
  _cl_command_node *command_node;

  if (command_queue == NULL)
    return CL_INVALID_COMMAND_QUEUE;
  
  if (kernel == NULL)
    return CL_INVALID_KERNEL;

  if (command_queue->context != kernel->context)
    return CL_INVALID_CONTEXT;

  if (work_dim < 1 ||
      work_dim > command_queue->device->max_work_item_dimensions)
    return CL_INVALID_WORK_DIMENSION;
  assert(command_queue->device->max_work_item_dimensions <= 3);

  command_node = (_cl_command_node*) malloc(sizeof(_cl_command_node));
  if (command_node == NULL)
    return CL_OUT_OF_HOST_MEMORY;

  if (global_work_offset != NULL)
    {
      offset_x = global_work_offset[0];
      offset_y = work_dim > 1 ? global_work_offset[1] : 1;
      offset_z = work_dim > 2 ? global_work_offset[2] : 1;
    }
  else
    {
      offset_x = 0;
      offset_y = 0;
      offset_z = 0;
    }
    
  global_x = global_work_size[0];
  global_y = work_dim > 1 ? global_work_size[1] : 1;
  global_z = work_dim > 2 ? global_work_size[2] : 1;

  if (global_x ==0 || global_y == 0 || global_z == 0)
    return CL_INVALID_GLOBAL_WORK_SIZE;

  local_x = local_work_size[0];
  local_y = work_dim > 1 ? local_work_size[1] : 1;
  local_z = work_dim > 2 ? local_work_size[2] : 1;

  if (local_x * local_y * local_z > command_queue->device->max_work_group_size)
    return CL_INVALID_WORK_GROUP_SIZE;

  if (local_x > command_queue->device->max_work_item_sizes[0] ||
      (work_dim > 1 &&
       local_y > command_queue->device->max_work_item_sizes[1]) ||
      (work_dim > 2 &&
       local_z > command_queue->device->max_work_item_sizes[2]))
    return CL_INVALID_WORK_ITEM_SIZE;

  if (global_x % local_x != 0 ||
      global_y % local_y != 0 ||
      global_z % local_z != 0)
    return CL_INVALID_WORK_GROUP_SIZE;

  tmpdir = mkdtemp(template);
  if (tmpdir == NULL)
    return CL_OUT_OF_HOST_MEMORY;

  if ((event_wait_list == NULL && num_events_in_wait_list > 0) ||
      (event_wait_list != NULL && num_events_in_wait_list == 0))
    return CL_INVALID_EVENT_WAIT_LIST;

  error = snprintf(kernel_filename, POCL_FILENAME_LENGTH,
		   "%s/kernel.bc",
		   tmpdir);
  if (error < 0)
    return CL_OUT_OF_HOST_MEMORY;

  kernel_file = fopen(kernel_filename, "w+");
  if (kernel_file == NULL)
    return CL_OUT_OF_HOST_MEMORY;

  n = fwrite(kernel->program->binary, 1,
	     kernel->program->binary_size, kernel_file);
  if (n < kernel->program->binary_size)
    return CL_OUT_OF_HOST_MEMORY;
  
  fclose(kernel_file);

  error = snprintf(parallel_filename, POCL_FILENAME_LENGTH,
		   "%s/parallel.bc",
		   tmpdir);
  if (error < 0)
    return CL_OUT_OF_HOST_MEMORY;
 
  if (stat(BUILDDIR "/scripts/" POCL_WORKGROUP, &buf) == 0)
    error = snprintf(command, COMMAND_LENGTH,
		     BUILDDIR "/scripts/" POCL_WORKGROUP " -k %s -x %zu -y %zu -z %zu -o %s %s",
		     kernel->function_name,
		     local_x, local_y, local_z,
		     parallel_filename, kernel_filename);
  else
    error = snprintf(command, COMMAND_LENGTH,
		     POCL_WORKGROUP " -k %s -x %zu -y %zu -z %zu -o %s %s",
		     kernel->function_name,
		     local_x, local_y, local_z,
		     parallel_filename, kernel_filename);
  if (error < 0)
    return CL_OUT_OF_HOST_MEMORY;

  error = system(command);
  if (error != 0)
    return CL_OUT_OF_RESOURCES;
  
  if (event != NULL)
    {
      *event = (cl_event)malloc(sizeof(struct _cl_event));
      if (*event == NULL)
        return CL_OUT_OF_HOST_MEMORY; 
      POCL_INIT_OBJECT(*event);
      (*event)->queue = command_queue;
    }

  pc.work_dim = work_dim;
  pc.num_groups[0] = global_x / local_x;
  pc.num_groups[1] = global_y / local_y;
  pc.num_groups[2] = global_z / local_z;
  pc.global_offset[0] = offset_x;
  pc.global_offset[1] = offset_y;
  pc.global_offset[2] = offset_z;

  
  command_node->type = CL_COMMAND_TYPE_RUN;
  command_node->command.run.data = command_queue->device->data;
  #warning parallel_filename is local
  command_node->command.run.file = parallel_filename;
  command_node->command.run.kernel = kernel;
  command_node->command.run.pc = pc;
  command_node->next = NULL; 
  
  POCL_RETAIN_OBJECT(command_queue);
  POCL_RETAIN_OBJECT(kernel);

  LL_APPEND(command_queue->root, command_node);
/*  command_queue->device->run(command_queue->device->data,
			     parallel_filename,
			     kernel,
			     &pc);
*/
  return CL_SUCCESS;
}
