4yue:
	cc src/main.c src/version.c -o 4yue

run: 4yue
	./4yue

clean:
	- rm -rf 4yue