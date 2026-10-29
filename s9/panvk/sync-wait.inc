/* Binary syncobj waits for PanVK's software timeline wrapper.
 * Included inside the isolated SweetGPU DRM adapter; not the working GL dylib.
 */
static int64_t
s9_monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t deadline,
              unsigned flags, uint32_t *first_signaled)
{
    (void)fd;
    if (!handles || !count ||
        (flags & ~(DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT))) {
        errno = EINVAL;
        return -EINVAL;
    }
    for (;;) {
        unsigned ready = 0, first = count;
        int invalid = 0, missing_fence = 0;
        for (unsigned i = 0; i < count; i++) {
            pthread_mutex_lock(&lock);
            struct ssync *s = ssync(handles[i]);
            int live = s != NULL, signaled = s && s->signaled;
            uint32_t seq = s ? s->seq : 0;
            pthread_mutex_unlock(&lock);
            if (!live) { invalid = 1; continue; }
            if (!seq && !signaled) { missing_fence = 1; continue; }
            if (signaled || (seq && fence_passed(seq))) {
                ready++;
                if (first == count) first = i;
            }
        }
        /* Validate every handle, even if an earlier one is signaled. */
        if (invalid || (missing_fence && !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT))) {
            errno = EINVAL;
            return -EINVAL;
        }
        if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) ? ready == count : ready > 0) {
            if (first_signaled) *first_signaled = first;
            return 0;
        }
        if (deadline != INT64_MAX && s9_monotonic_ns() >= deadline) {
            errno = ETIME;
            return -ETIME;
        }
        /* Poll briefly: GPU completion and CPU signals can change independently.
         * Never hold the table lock while polling the GPU or sleeping. */
        struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&pause, NULL);
    }
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
    (void)fd;
    if (!handles && count) { errno = EINVAL; return -EINVAL; }
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < count; i++) {
        if (!ssync(handles[i])) {
            pthread_mutex_unlock(&lock);
            errno = EINVAL;
            return -EINVAL;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        struct ssync *s = ssync(handles[i]);
        s->seq = 0;
        s->signaled = 1;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

/* Native timelines are not advertised. PanVK v6 uses vk_sync_timeline. */
int drmSyncobjTimelineSignal(int fd, const uint32_t *h, uint64_t *p, uint32_t n)
{
    (void)fd; (void)h; (void)p; (void)n;
    errno = ENOTSUP; return -ENOTSUP;
}
int drmSyncobjQuery2(int fd, uint32_t *h, uint64_t *p, uint32_t n, uint32_t flags)
{
    (void)fd; (void)h; (void)p; (void)n; (void)flags;
    errno = ENOTSUP; return -ENOTSUP;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
    (void)fd;
    if (!handles && count) { errno = EINVAL; return -EINVAL; }
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < count; i++) {
        if (!ssync(handles[i])) {
            pthread_mutex_unlock(&lock);
            errno = EINVAL; return -EINVAL;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        struct ssync *s = ssync(handles[i]);
        s->seq = 0;
        s->signaled = 0;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
