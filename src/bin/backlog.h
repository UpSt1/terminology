#ifndef TERMINOLOGY_BACKLOG_H_ 
#define TERMINOLOGY_BACKLOG_H_ 1

void termpty_save_register(Termpty *ty);
void termpty_save_unregister(Termpty *ty);
Termsave *termpty_save_extract(Termsave *ts);
Termsave *termpty_save_new(Termpty *ty, Termsave *ts, int w);
void termpty_save_free(Termpty *ty, Termsave *ts);
Termsave *termpty_save_expand(Termpty *ty, Termsave *ts,
                              Termcell *cells, size_t delta);

void       termpty_backlog_lock(void);
void       termpty_backlog_unlock(void);

void
termpty_clear_backlog(Termpty *ty);
void
termpty_backlog_free(Termpty *ty);
void
termpty_backlog_size_set(Termpty *ty, size_t size);
ssize_t
termpty_backlog_length(Termpty *ty);

int64_t
termpty_backlog_memory_get(void);

#define BACKLOG_ROW_GET(Ty, Y) \
   (&Ty->back[(Ty->backsize - 1 + ty->backpos - Y) % Ty->backsize])

#endif
