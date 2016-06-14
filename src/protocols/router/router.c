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

#include "router.h"
#include "xrouter.h"

#include "../../nn.h"
#include "../../router.h"

#include "../../utils/cont.h"
#include "../../utils/alloc.h"
#include "../../utils/err.h"
#include "../../utils/list.h"

struct nn_router {
    struct nn_xrouter xrouter;
};

/*  Private functions. */
static void nn_router_init (struct nn_router *self,
    const struct nn_sockbase_vfptr *vfptr, void *hint);
static void nn_router_term (struct nn_router *self);

/*  Implementation of nn_sockbase's virtual functions. */
static void nn_router_destroy (struct nn_sockbase *self);
static int nn_router_send (struct nn_sockbase *self, struct nn_msg *msg);
static int nn_router_recv (struct nn_sockbase *self, struct nn_msg *msg);
static const struct nn_sockbase_vfptr nn_router_sockbase_vfptr = {
    NULL,
    nn_router_destroy,
    nn_xrouter_add,
    nn_xrouter_rm,
    nn_xrouter_in,
    nn_xrouter_out,
    nn_xrouter_events,
    nn_router_send,
    nn_router_recv,
    nn_xrouter_setopt,
    nn_xrouter_getopt
};

static void nn_router_init (struct nn_router *self,
    const struct nn_sockbase_vfptr *vfptr, void *hint)
{
    nn_xrouter_init (&self->xrouter, vfptr, hint);
}

static void nn_router_term (struct nn_router *self)
{
    nn_xrouter_term (&self->xrouter);
}

static void nn_router_destroy (struct nn_sockbase *self)
{
    struct nn_router *router;

    router = nn_cont (self, struct nn_router, xrouter.sockbase);

    nn_router_term (router);
    nn_free (router);
}

static int nn_router_send (struct nn_sockbase *self, struct nn_msg *msg)
{
    int rc;
    struct nn_router *router;

    router = nn_cont (self, struct nn_router, xrouter.sockbase);

    /*  Check for malformed messages. */
    if (nn_chunkref_size (&msg->sphdr))
        return -EINVAL;

    /*  Send the message. */
    rc = nn_xrouter_send (&router->xrouter.sockbase, msg);
    errnum_assert (rc == 0, -rc);

    return 0;
}

static int nn_router_recv (struct nn_sockbase *self, struct nn_msg *msg)
{
    int rc;
    struct nn_router *router;

    router = nn_cont (self, struct nn_router, xrouter.sockbase);

    /*  Get next message. */
    rc = nn_xrouter_recv (&router->xrouter.sockbase, msg);
    if (nn_slow (rc == -EAGAIN))
        return -EAGAIN;
    errnum_assert (rc == 0, -rc);
    nn_assert (nn_chunkref_size (&msg->sphdr) == sizeof (uint64_t));

    /*  Discard the header. */
    nn_chunkref_term (&msg->sphdr);
    nn_chunkref_init (&msg->sphdr, 0);
    
    return 0;
}

static int nn_router_create (void *hint, struct nn_sockbase **sockbase)
{
    struct nn_router *self;

    self = nn_alloc (sizeof (struct nn_router), "socket (router)");
    alloc_assert (self);
    nn_router_init (self, &nn_router_sockbase_vfptr, hint);
    *sockbase = &self->xrouter.sockbase;

    return 0;
}

static struct nn_socktype nn_router_socktype_struct = {
    AF_SP,
    NN_ROUTER,
    0,
    nn_router_create,
    nn_xrouter_ispeer,
    NN_LIST_ITEM_INITIALIZER
};

struct nn_socktype *nn_router_socktype = &nn_router_socktype_struct;

