/*
    Copyright (c) 2013 Martin Sustrik  All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "mdist.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/attr.h"

#include <stddef.h>

void nn_mdist_init (struct nn_mdist *self)
{
    self->count = 0;
    nn_list_init (&self->pipes);
}

void nn_mdist_term (struct nn_mdist *self)
{
    nn_assert (self->count == 0);
    nn_list_term (&self->pipes);
}

void nn_mdist_add (NN_UNUSED struct nn_mdist *self,
    struct nn_mdist_data *data, struct nn_pipe *pipe)
{
    data->pipe = pipe;
    data->usedFlag = 0;
    nn_list_item_init (&data->item);
}

void nn_mdist_rm (struct nn_mdist *self, struct nn_mdist_data *data)
{
    if (nn_list_item_isinlist (&data->item)) {
        --self->count;
        nn_list_erase (&self->pipes, &data->item);
    }
    nn_list_item_term (&data->item);
}

void nn_mdist_out (struct nn_mdist *self, struct nn_mdist_data *data)
{
    if (data->usedFlag == 0)
    {
        nn_mdist_send_name(self, data->pipe);
        data->usedFlag = 1;  
        //before adding it to the list, check the list for the same name router    
    }
    else
    {
        ++self->count;
        nn_list_insert (&self->pipes, &data->item, nn_list_end (&self->pipes));
        
    }
}

int nn_mdist_send (struct nn_mdist *self, struct nn_msg *msg,
    struct nn_pipe *exclude)
{
    int rc;
    struct nn_list_item *it;
    struct nn_mdist_data *data;
    struct nn_msg copy;

    uint8_t *msg_data;
    int sz;

    msg_data = nn_chunkref_data (&msg->body);
    sz   = nn_chunkref_size (&msg->body);

    /*  In the specific case when there are no outbound pipes. There's nowhere
        to send the message to. Deallocate it. */
    if (nn_slow (self->count) == 0) {
        nn_msg_term (msg);
        return 0;
    }

    it = nn_list_begin (&self->pipes);
    while (it != nn_list_end (&self->pipes)) 
    {
       data = nn_cont (it, struct nn_mdist_data, item);
       //size must be changed
       rc = strncmp(data->pipeName, msg_data, 4);
       if (rc == 0)
       {
            rc = nn_pipe_send (data->pipe, msg);
            if (rc & NN_PIPE_RELEASE) 
            {
                --self->count;
                it = nn_list_erase (&self->pipes, it);
                continue;
            }
       }
       it = nn_list_next (&self->pipes, it);
    }
    nn_msg_term (msg);

    return 0;
}

int nn_mdist_has_pipe (struct nn_mdist *self, struct nn_pipe *pipe, void** hint)
{
    struct nn_list_item *it;
    struct nn_mdist_data *data;
    int rc;

    it = nn_list_begin (&self->pipes);
    while (it != nn_list_end (&self->pipes)) 
    {
        data = nn_cont (it, struct nn_mdist_data, item);
        if (nn_fast (data->pipe == pipe)) 
        {
            rc = (data->usedFlag & 0xF0) ? 1 : 0;
            *(hint) = data;
            return rc;
        }
        it = nn_list_next (&self->pipes, it);
    }

    return -1;
}

int nn_mdist_name_pipe (struct nn_mdist_data *data, char *name)
{
    strncpy (data->pipeName, name, 4);
    data->usedFlag |= 0xF0;
    return 0;
}

int nn_mdist_send_name (struct nn_mdist *self, struct nn_pipe *pipe)
{
    struct nn_iovec iov;
    struct nn_msghdr hdr;
    struct nn_cmsghdr *cmsg;
    struct nn_msg msg;
    void *chunk;
    size_t sz;
    int rc;

    iov.iov_base = self->pipe_name;
    iov.iov_len = 5;

    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = NULL;
    hdr.msg_controllen = 0;

    sz = 5;
    nn_msg_init (&msg, sz);
    memcpy (((uint8_t*) nn_chunkref_data (&msg.body)) ,
                iov.iov_base, iov.iov_len);

    rc = nn_pipe_send (pipe, &msg);
    errnum_assert (rc >= 0, -rc);
    
    return rc;
}

