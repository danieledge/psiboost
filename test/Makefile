# PsiBoost's link test logic on a PC, against server/stdsvc.py
test: sptest_host
	@python3 ../server/stdsvc.py --host 127.0.0.1 --base 17100 > stdsvc.log 2>&1 & echo $$! > stdsvc.pid; sleep 0.7; \
	./sptest_host 17100; r=$$?; kill $$(cat stdsvc.pid); rm -f stdsvc.pid; exit $$r
sptest_host: sptest_host.c ../sptest.c ../sptest.h
	gcc -O1 -Wall -Wextra -fsanitize=address,undefined -o $@ sptest_host.c ../sptest.c
clean:
	rm -f sptest_host stdsvc.log stdsvc.pid
