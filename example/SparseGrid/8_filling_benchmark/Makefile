include ../../example.mk
include ../../common.mk

OBJ = main.o
gray_scott_sparse_opt_test: OPT += -DTEST_RUN
gray_scott_sparse_opt_test: gray_scott_sparse_opt

%.o: %.cpp
	$(CC) -mavx $(OPT) -c -o $@ $< $(INCLUDE_PATH)

gray_scott_sparse_opt: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS_PATH) $(LIBS)

all: gray_scott_sparse_opt

run: gray_scott_sparse_opt_test
	mpirun --oversubscribe -np 4 ./gray_scott_sparse_opt

.PHONY: clean all run

clean:
	rm -f *.o *~ core gray_scott_sparse_opt

