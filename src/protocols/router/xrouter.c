/*
    Copyright (c) 2013-2014 Martin Sustrik  All rights reserved.

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

#include "xrouter.h"

#include "../../nn.h"
#include "../../router.h"

#include "../../utils/err.h"
#include "../../utils/cont.h"
#include "../../utils/fast.h"
#include "../../utils/alloc.h"
#include "../../utils/list.h"
#include "../../utils/attr.h"

#include <stddef.h>
#include <string.h>

/*  To make the algorithm super efficient we directly cast pipe pointers to
    pipe IDs (rather than maintaining a hash table). For this to work, it is
    neccessary for the pointer to fit in 64-bit ID. */
CT_ASSERT (sizeof (uint64_t) >= sizeof (struct nn_pipe*));

/*  Implementation of nn_sockbase's virtual functions. */
static void nn_xrouter_destroy (struct nn_sockbase *self);
static const struct nn_sockbase_vfptr nn_xrouter_sockbase_vfptr = {
    NULL,
    nn_xrouter_destroy,
    nn_xrouter_add,
    nn_xrouter_rm,
    nn_xrouter_in,
    nn_xrouter_out,
    nn_xrouter_events,
    nn_xrouter_send,
    nn_xrouter_recv,
    nn_xrouter_setopt,
    nn_xrouter_getopt
};

void nn_xrouter_init (struct nn_xrouter *self,
    const struct nn_sockbase_vfptr *vfptr, void *hint)
{
    nn_sockbase_init (&self->sockbase, vfptr, hint);
    nn_mdist_init (&self->outpipes);
    nn_fq_init (&self->inpipes);
}

void nn_xrouter_term (struct nn_xrouter *self)
{
    nn_fq_term (&self->inpipes);
    nn_mdist_term (&self->outpipes);
    nn_sockbase_term (&self->sockbase);
}

static void nn_xrouter_destroy (struct nn_sockbase *self)
{
    struct nn_xrouter *xrouter;

    xrouter = nn_cont (self, struct nn_xrouter, sockbase);

    nn_xrouter_term (xrouter);
    nn_free (xrouter);
}

int nn_xrouter_add (struct nn_sockbase *self, struct nn_pipe *pipe)
{
    struct nn_xrouter *xrouter;
    struct nn_xrouter_data *data;
    int rcvprio;
    size_t sz;

    xrouter = nn_cont (self, struct nn_xrouter, sockbase);

    sz = sizeof (rcvprio);
    nn_pipe_getopt (pipe, NN_SOL_SOCKET, NN_RCVPRIO, &rcvprio, &sz);
    nn_assert (sz == sizeof (rcvprio));
    nn_assert (rcvprio >= 1 && rcvprio <= 16);

    data = nn_alloc (sizeof (struct nn_xrouter_data), "pipe data (xrouter)");
    alloc_assert (data);
    nn_fq_add (&xrouter->inpipes, &data->initem, pipe, rcvprio);
    nn_mdist_add (&xrouter->outpipes, &data->outitem, pipe);
    nn_pipe_setdata (pipe, data);

    return 0;
}

void nn_xrouter_rm (struct nn_sockbase *self, struct nn_pipe *pipe)
{
    struct nn_xrouter *xrouter;
    struct nn_xrouter_data *data;

    xrouter = nn_cont (self, struct nn_xrouter, sockbase);
    data = nn_pipe_getdata (pipe);

    nn_fq_rm (&xrouter->inpipes, &data->initem);
    nn_mdist_rm (&xrouter->outpipes, &data->outitem);

    nn_free (data);
}

void nn_xrouter_in (struct nn_sockbase *self, struct nn_pipe *pipe)
{
    struct nn_xrouter *xrouter;
    struct nn_xrouter_data *data;

    xrouter = nn_cont (self, struct nn_xrouter, sockbase);
    data = nn_pipe_getdata (pipe);

    nn_fq_in (&xrouter->inpipes, &data->initem);
}

void nn_xrouter_out (struct nn_sockbase *self, struct nn_pipe *pipe)
{
    struct nn_xrouter *xrouter;
    struct nn_xrouter_data *data;

    xrouter = nn_cont (self, struct nn_xrouter, sockbase);
    data = nn_pipe_getdata (pipe);

    nn_mdist_out (&xrouter->outpipes, &data->outitem);
}

int nn_xrouter_events (struct nn_sockbase *self)
{
    return (nn_fq_can_recv (&nn_cont (self, struct nn_xrouter,
        sockbase)->inpipes) ? NN_SOCKBASE_EVENT_IN : 0) | NN_SOCKBASE_EVENT_OUT;
}

int nn_xrouter_send (struct nn_sockbase *self, struct nn_msg *msg)
{
    size_t hdrsz;
    struct nn_pipe *exclude;

    hdrsz = nn_chunkref_size (&msg->sphdr);
    if (hdrsz == 0)
        exclude = NULL;
    else if (hdrsz == sizeof (uint64_t)) {
        memcpy (&exclude, nn_chunkref_data (&msg->sphdr), sizeof (exclude));
        nn_chunkref_term (&msg->sphdr);
        nn_chunkref_init (&msg->sphdr, 0);
    }
    else
        return -EINVAL;

    return nn_mdist_send (&nn_cont (self, struct nn_xrouter, sockbase)->outpipes,
        msg, exclude);
}

int nn_xrouter_recv (struct nn_sockbase *self, struct nn_msg *msg)
{
    int rc;
    struct nn_xrouter *xrouter;
    struct nn_pipe *pipe;
    uint8_t *msg_data;
    struct nn_mdist_data *pipe_data;
    int sz;

    xrouter = nn_cont (self, struct nn_xrouter, sockbase);

    while (1) 
    {

        /*  Get next message in fair-queued manner. */
        rc = nn_fq_recv (&xrouter->inpipes, msg, &pipe);
        if (nn_slow (rc < 0))
            return rc;

        rc = nn_mdist_has_pipe (&nn_cont (self, struct nn_xrouter, sockbase)->outpipes, pipe, &pipe_data);
        if (rc == 0)
        {
            msg_data = nn_chunkref_data (&msg->body);
            sz   = nn_chunkref_size (&msg->body);
            rc = nn_mdist_name_pipe (pipe_data, msg_data);
            nn_msg_term (msg);
            continue;          
        }
        /*  The message should have no header. Drop malformed messages. */
        if (nn_chunkref_size (&msg->sphdr) == 0)
            break;
        nn_msg_term (msg);
    }

    /*  Add pipe ID to the message header. */
    nn_chunkref_term (&msg->sphdr);
    nn_chunkref_init (&msg->sphdr, sizeof (uint64_t));
    memset (nn_chunkref_data (&msg->sphdr), 0, sizeof (uint64_t));
    memcpy (nn_chunkref_data (&msg->sphdr), &pipe, sizeof (pipe));

    return 0;
}

int nn_xrouter_setopt (struct nn_sockbase *self, int level, int option,
        const void *optval, size_t optvallen)
{
    int rc;
    struct nn_xrouter *xrouter;
    xrouter =  nn_cont (self, struct nn_xrouter, sockbase);

    if (option == NN_ROUTER_NAME)
    {
        strncpy ((xrouter->outpipes).pipe_name, optval, 4);
        return 0;
    }
    return -ENOPROTOOPT;
}

int nn_xrouter_getopt (NN_UNUSED struct nn_sockbase *self, NN_UNUSED int level,
    NN_UNUSED int option,
    NN_UNUSED void *optval, NN_UNUSED size_t *optvallen)
{
    return -ENOPROTOOPT;
}

static int nn_xrouter_create (void *hint, struct nn_sockbase **sockbase)
{
    struct nn_xrouter *self;

    self = nn_alloc (sizeof (struct nn_xrouter), "socket (router)");
    alloc_assert (self);
    nn_xrouter_init (self, &nn_xrouter_sockbase_vfptr, hint);
    *sockbase = &self->sockbase;

    return 0;
}

int nn_xrouter_ispeer (int socktype)
{
    return socktype == NN_ROUTER ? 1 : 0;
}

static struct nn_socktype nn_xrouter_socktype_struct = {
    AF_SP_RAW,
    NN_ROUTER,
    0,
    nn_xrouter_create,
    nn_xrouter_ispeer,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_socktype *nn_xrouter_socktype = &nn_xrouter_socktype_struct;

