CC ?= cc
WARNINGS = -Wall -Wextra -Wpedantic -Werror
CFLAGS ?= -std=c11 -O2 $(WARNINGS)
GEOMETRY_DIR = ../geometry
COMMON = common/protocol.c
SESSION = fake-server/pattern.c fake-server/session.c
BINDIR = build

.PHONY: all test integration input-integration sanitize clean

all: $(BINDIR)/fake-server $(BINDIR)/protocol_test $(BINDIR)/session_test $(BINDIR)/scripted-agent $(BINDIR)/input-agent $(BINDIR)/stage5-agent $(BINDIR)/stage6-agent $(BINDIR)/stage7a-agent

$(BINDIR):
	mkdir -p $@

$(BINDIR)/fake-server: $(COMMON) $(SESSION) fake-server/main.c $(GEOMETRY_DIR)/geometry.c | $(BINDIR)
	$(CC) $(CFLAGS) -I$(GEOMETRY_DIR) -Icommon $^ -o $@

$(BINDIR)/protocol_test: $(COMMON) tests/protocol_test.c | $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@

$(BINDIR)/session_test: $(COMMON) $(SESSION) tests/session_test.c $(GEOMETRY_DIR)/geometry.c | $(BINDIR)
	$(CC) $(CFLAGS) -I$(GEOMETRY_DIR) -Icommon $^ -o $@

$(BINDIR)/scripted-agent: $(COMMON) tests/scripted_agent.c | $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@

$(BINDIR)/input-agent: $(COMMON) tests/input_agent.c | $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@

$(BINDIR)/stage5-agent: $(COMMON) tests/stage5_agent.c | $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@

$(BINDIR)/stage6-agent: $(COMMON) tests/stage6_agent.c | $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@

$(BINDIR)/stage7a-agent: $(COMMON) tests/stage7a_agent.c | $(BINDIR)
	$(CC) $(CFLAGS) $^ -o $@

test: $(BINDIR)/protocol_test $(BINDIR)/session_test
	./$(BINDIR)/protocol_test
	./$(BINDIR)/session_test
	$(MAKE) -C $(GEOMETRY_DIR) test

integration: $(BINDIR)/fake-server $(BINDIR)/scripted-agent
	./$(BINDIR)/fake-server --listen 127.0.0.1 --port 44601 --script-stage2 --trace-present > scripted-server.log 2>&1 & crosswin_server_pid=$$!; ./$(BINDIR)/scripted-agent 44601; test_status=$$?; wait $$crosswin_server_pid; server_status=$$?; test $$test_status -eq 0 -a $$server_status -eq 0

input-integration: $(BINDIR)/fake-server $(BINDIR)/input-agent
	./$(BINDIR)/fake-server --listen 127.0.0.1 --port 44602 --interactive --trace-present --trace-input > input-server.log 2>&1 & crosswin_input_server_pid=$$!; ./$(BINDIR)/input-agent; test_status=$$?; wait $$crosswin_input_server_pid; server_status=$$?; test $$test_status -eq 0 -a $$server_status -eq 0

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-std=c11 -O1 -g $(WARNINGS) -fsanitize=address,undefined -fno-omit-frame-pointer' test
	$(MAKE) -C $(GEOMETRY_DIR) sanitize

clean:
	rm -rf $(BINDIR)
	rm -f scripted-server.log input-server.log
