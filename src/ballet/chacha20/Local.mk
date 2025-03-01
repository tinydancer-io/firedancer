ifdef FD_HAS_INT128
$(call add-hdrs,fd_chacha20.h fd_chacha20rng.h)
$(call add-objs,fd_chacha20 fd_chacha20rng,fd_ballet)
$(call make-unit-test,test_chacha20,test_chacha20,fd_ballet fd_util)
$(call make-unit-test,test_chacha20rng,test_chacha20rng,fd_ballet fd_util)
$(call make-unit-test,test_chacha20rng_roll,test_chacha20rng_roll,fd_ballet fd_util)
$(call run-unit-test,test_chacha20)
$(call run-unit-test,test_chacha20rng)
endif
