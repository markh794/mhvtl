/*
 * Unit tests for vtlcmd command validation logic.
 *
 * Tests Check_Params() and Check_DeviceCommand() from vtlcmd.c directly,
 * now that they return error strings instead of calling exit().
 *
 * We compile vtlcmd.c with -Dmain=vtlcmd_main to avoid the duplicate
 * main symbol, and stub out the IPC/filesystem dependencies.
 */
#include "acutest.h"

#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>

#include "vtl_common.h"
#include "vtllib.h"

/* Device type constants matching vtlcmd.c */
#define TYPE_UNKNOWN 0
#define TYPE_LIBRARY 1
#define TYPE_DRIVE   2

/* Stubs for vtlcmd.c dependencies we don't need for validation tests */
long my_id = 1;
char home_directory[HOME_DIR_PATH_SZ + 1] = "/tmp";
void find_media_home_directory(char *a, long b) { (void)a; (void)b; }
int get_config(char *a, conf_file b, long c) { (void)a; (void)b; (void)c; return 0; }
void syslog(int priority, const char *fmt, ...) { (void)priority; (void)fmt; }

/* Declarations for vtlcmd.c functions under test */
extern const char *Check_Params(int argc, char **argv);
extern const char *Check_DeviceCommand(const char *buf, int device_type);

/* ---- Check_Params tests ---- */

void test_params_missing_args(void) {
	/* No args at all — just program name */
	char *argv[] = {"vtlcmd"};
	const char *err = Check_Params(1, argv);
	TEST_CHECK(err != NULL);
	TEST_MSG("Should fail with no arguments, got: %s", err ? err : "(null)");
}

void test_params_non_numeric_device(void) {
	char *argv[] = {"vtlcmd", "abc", "verbose"};
	const char *err = Check_Params(3, argv);
	TEST_CHECK(err != NULL);
	TEST_MSG("Non-numeric device should fail, got: %s", err ? err : "(null)");
}

void test_params_missing_command(void) {
	/* Device number but no command */
	char *argv[] = {"vtlcmd", "10"};
	const char *err = Check_Params(2, argv);
	TEST_CHECK(err != NULL);
	TEST_MSG("Missing command should fail, got: %s", err ? err : "(null)");
}

void test_params_global_commands_valid(void) {
	struct { int argc; char *argv[6]; } cases[] = {
		{3, {"vtlcmd", "10", "verbose"}},
		{3, {"vtlcmd", "10", "dump"}},
		{3, {"vtlcmd", "10", "debug"}},
		{3, {"vtlcmd", "10", "exit"}},
		{3, {"vtlcmd", "10", "InquiryDataChange"}},
		{4, {"vtlcmd", "10", "TapeAlert", "ff"}},
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const char *err = Check_Params(cases[i].argc, cases[i].argv);
		TEST_CHECK(err == NULL);
		TEST_MSG("'%s' should be valid, got error: %s",
				 cases[i].argv[2], err ? err : "(null)");
	}
}

void test_params_global_commands_extra_args(void) {
	/* verbose with extra arg should fail */
	char *argv[] = {"vtlcmd", "10", "verbose", "extra"};
	const char *err = Check_Params(4, argv);
	TEST_CHECK(err != NULL);
	TEST_MSG("verbose with extra arg should fail");
}

void test_params_load_variants(void) {
	/* load <barcode> — valid */
	{
		char *argv[] = {"vtlcmd", "10", "load", "TAPE001"};
		TEST_CHECK(Check_Params(4, argv) == NULL);
	}
	/* load map <barcode> — valid */
	{
		char *argv[] = {"vtlcmd", "10", "load", "map", "TAPE001"};
		TEST_CHECK(Check_Params(5, argv) == NULL);
	}
	/* load alone — invalid */
	{
		char *argv[] = {"vtlcmd", "10", "load"};
		TEST_CHECK(Check_Params(3, argv) != NULL);
	}
	/* load map without barcode — invalid */
	{
		char *argv[] = {"vtlcmd", "10", "load", "map"};
		TEST_CHECK(Check_Params(4, argv) != NULL);
	}
}

void test_params_unload(void) {
	/* unload <barcode> — valid */
	{
		char *argv[] = {"vtlcmd", "10", "unload", "TAPE001"};
		TEST_CHECK(Check_Params(4, argv) == NULL);
	}
	/* unload alone — invalid */
	{
		char *argv[] = {"vtlcmd", "10", "unload"};
		TEST_CHECK(Check_Params(3, argv) != NULL);
	}
}

void test_params_compression(void) {
	/* compression zlib — valid */
	{
		char *argv[] = {"vtlcmd", "10", "compression", "zlib"};
		TEST_CHECK(Check_Params(4, argv) == NULL);
	}
	/* compression alone — invalid */
	{
		char *argv[] = {"vtlcmd", "10", "compression"};
		TEST_CHECK(Check_Params(3, argv) != NULL);
	}
}

void test_params_delay(void) {
	/* delay load 5 — valid */
	{
		char *argv[] = {"vtlcmd", "10", "delay", "load", "5"};
		TEST_CHECK(Check_Params(5, argv) == NULL);
	}
	/* delay alone — invalid */
	{
		char *argv[] = {"vtlcmd", "10", "delay"};
		TEST_CHECK(Check_Params(3, argv) != NULL);
	}
	/* delay load without value — invalid */
	{
		char *argv[] = {"vtlcmd", "10", "delay", "load"};
		TEST_CHECK(Check_Params(4, argv) != NULL);
	}
}

void test_params_append_only(void) {
	/* Append Only Yes — valid */
	{
		char *argv[] = {"vtlcmd", "10", "Append", "Only", "Yes"};
		TEST_CHECK(Check_Params(5, argv) == NULL);
	}
	/* Append alone — invalid */
	{
		char *argv[] = {"vtlcmd", "10", "Append"};
		TEST_CHECK(Check_Params(3, argv) != NULL);
	}
}

void test_params_library_commands(void) {
	struct { int argc; char *argv[6]; } cases[] = {
		{4, {"vtlcmd", "10", "add", "slot"}},
		{3, {"vtlcmd", "10", "online"}},
		{3, {"vtlcmd", "10", "offline"}},
		{4, {"vtlcmd", "10", "list", "map"}},
		{4, {"vtlcmd", "10", "empty", "map"}},
		{4, {"vtlcmd", "10", "open", "map"}},
		{4, {"vtlcmd", "10", "close", "map"}},
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const char *err = Check_Params(cases[i].argc, cases[i].argv);
		TEST_CHECK(err == NULL);
		TEST_MSG("'%s %s' should be valid, got error: %s",
				 cases[i].argv[2],
				 cases[i].argc > 3 ? cases[i].argv[3] : "",
				 err ? err : "(null)");
	}
}

void test_params_unknown_command(void) {
	char *argv[] = {"vtlcmd", "10", "bogus"};
	const char *err = Check_Params(3, argv);
	TEST_CHECK(err != NULL);
	TEST_MSG("Unknown command should fail");
}

void test_params_tapealert_hex_validation(void) {
	/* Valid hex */
	{
		char *argv[] = {"vtlcmd", "10", "TapeAlert", "ff00"};
		TEST_CHECK(Check_Params(4, argv) == NULL);
	}
	/* Invalid hex */
	{
		char *argv[] = {"vtlcmd", "10", "TapeAlert", "xyz"};
		const char *err = Check_Params(4, argv);
		TEST_CHECK(err != NULL);
		TEST_MSG("Non-hex TapeAlert value should fail");
	}
}

void test_params_inquiry_data_change_case(void) {
	/* Case-insensitive match via strncasecmp */
	{
		char *argv[] = {"vtlcmd", "10", "inquirydatachange"};
		TEST_CHECK(Check_Params(3, argv) == NULL);
	}
	/* Typo should fail — "InquriyDataChange" is only 17 chars but wrong */
	{
		char *argv[] = {"vtlcmd", "10", "InquriyDataChange"};
		/* This passes strncasecmp check because first 17 chars differ */
		const char *err = Check_Params(3, argv);
		/* The strncasecmp("InquriyDataChange", "InquiryDataChange", 17) fails
		 * because 'r' vs 'i' at position 5. So this should NOT match. */
		TEST_CHECK(err != NULL);
		TEST_MSG("Typo 'InquriyDataChange' should not be accepted by Check_Params");
	}
}

/* ---- Check_DeviceCommand tests ---- */

void test_devcmd_library_valid(void) {
	const char *valid[] = {
		"online",
		"add slot 1234 ",
		"offline",
		"open map 1 ",
		"close map 1 ",
		"empty map 1 ",
		"list map ",
		"load map TAPE001 ",
		"verbose 1 ",
		"debug 3 ",
		"exit ",
		"TapeAlert 0x00 ",
		"InquiryDataChange ",
	};

	for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
		const char *err = Check_DeviceCommand(valid[i], TYPE_LIBRARY);
		TEST_CHECK(err == NULL);
		TEST_MSG("'%s' should be valid library command, got: %s",
				 valid[i], err ? err : "(null)");
	}
}

void test_devcmd_library_invalid(void) {
	const char *invalid[] = {
		"unload ",
		"compression zlib ",
		"dump ",
		"append Only Yes ",
		"delay load 5 ",
		"bogus ",
	};

	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		const char *err = Check_DeviceCommand(invalid[i], TYPE_LIBRARY);
		TEST_CHECK(err != NULL);
		TEST_MSG("'%s' should NOT be valid library command", invalid[i]);
	}
}

void test_devcmd_drive_valid(void) {
	const char *valid[] = {
		"load TAPE001 ",
		"unload ",
		"verbose 2 ",
		"debug 1 ",
		"dump ",
		"exit ",
		"compression zlib ",
		"TapeAlert 0xff ",
		"InquiryDataChange ",
		"append Only Yes ",
		"Append Only No ",
		"APPEND Only Yes ",
		"delay load 5 ",
		"delay unload 3 ",
		"delay rewind 2 ",
		"delay position 1 ",
		"delay thread 4 ",
		"Delay Load 10 ",
	};

	for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
		const char *err = Check_DeviceCommand(valid[i], TYPE_DRIVE);
		TEST_CHECK(err == NULL);
		TEST_MSG("'%s' should be valid drive command, got: %s",
				 valid[i], err ? err : "(null)");
	}
}

void test_devcmd_drive_invalid(void) {
	const char *invalid[] = {
		"online ",
		"add slot 1 ",
		"offline ",
		"open map 1 ",
		"close map 1 ",
		"empty map 1 ",
		"list map ",
		"bogus ",
	};

	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		const char *err = Check_DeviceCommand(invalid[i], TYPE_DRIVE);
		TEST_CHECK(err != NULL);
		TEST_MSG("'%s' should NOT be valid drive command", invalid[i]);
	}
}

void test_devcmd_unknown_type_allows_all(void) {
	/* TYPE_UNKNOWN should not reject anything (neither branch taken) */
	const char *err = Check_DeviceCommand("bogus", TYPE_UNKNOWN);
	TEST_CHECK(err == NULL);
	TEST_MSG("TYPE_UNKNOWN should not reject any command");
}

void test_devcmd_shared_commands(void) {
	const char *shared[] = {
		"verbose ",
		"debug ",
		"exit ",
		"TapeAlert ",
		"InquiryDataChange ",
	};

	for (size_t i = 0; i < sizeof(shared) / sizeof(shared[0]); i++) {
		TEST_CHECK(Check_DeviceCommand(shared[i], TYPE_LIBRARY) == NULL);
		TEST_MSG("'%s' should be valid for library", shared[i]);
		TEST_CHECK(Check_DeviceCommand(shared[i], TYPE_DRIVE) == NULL);
		TEST_MSG("'%s' should be valid for drive", shared[i]);
	}
}

void test_devcmd_inquiry_spelling(void) {
	/* Correct spelling */
	TEST_CHECK(Check_DeviceCommand("InquiryDataChange ", TYPE_LIBRARY) == NULL);
	TEST_CHECK(Check_DeviceCommand("InquiryDataChange ", TYPE_DRIVE) == NULL);

	/* Typo must be rejected */
	TEST_CHECK(Check_DeviceCommand("InquriyDataChange ", TYPE_LIBRARY) != NULL);
	TEST_CHECK(Check_DeviceCommand("InquriyDataChange ", TYPE_DRIVE) != NULL);

	/* Case sensitivity: Check_DeviceCommand uses strncmp (case-sensitive) */
	TEST_CHECK(Check_DeviceCommand("inquirydatachange ", TYPE_LIBRARY) != NULL);
	TEST_MSG("Lowercase should not match (strncmp is case-sensitive)");
}

void test_devcmd_prefix_matching_quirks(void) {
	/* "load" with 4-char prefix match means "loader" also matches for drive */
	TEST_CHECK(Check_DeviceCommand("loader ", TYPE_DRIVE) == NULL);
	TEST_MSG("'loader' matches 'load' prefix for drive");

	/* "exit" with 4-char prefix match */
	TEST_CHECK(Check_DeviceCommand("exiting ", TYPE_DRIVE) == NULL);
	TEST_MSG("'exiting' matches 'exit' prefix");

	/* Very long command string shouldn't crash */
	char longcmd[512];
	memset(longcmd, 'x', sizeof(longcmd) - 1);
	longcmd[sizeof(longcmd) - 1] = '\0';
	TEST_CHECK(Check_DeviceCommand(longcmd, TYPE_LIBRARY) != NULL);
	TEST_CHECK(Check_DeviceCommand(longcmd, TYPE_DRIVE) != NULL);
}

TEST_LIST = {
	/* Check_Params tests */
	{"params_missing_args", test_params_missing_args},
	{"params_non_numeric_device", test_params_non_numeric_device},
	{"params_missing_command", test_params_missing_command},
	{"params_global_commands_valid", test_params_global_commands_valid},
	{"params_global_commands_extra_args", test_params_global_commands_extra_args},
	{"params_load_variants", test_params_load_variants},
	{"params_unload", test_params_unload},
	{"params_compression", test_params_compression},
	{"params_delay", test_params_delay},
	{"params_append_only", test_params_append_only},
	{"params_library_commands", test_params_library_commands},
	{"params_unknown_command", test_params_unknown_command},
	{"params_tapealert_hex_validation", test_params_tapealert_hex_validation},
	{"params_inquiry_data_change_case", test_params_inquiry_data_change_case},
	/* Check_DeviceCommand tests */
	{"devcmd_library_valid", test_devcmd_library_valid},
	{"devcmd_library_invalid", test_devcmd_library_invalid},
	{"devcmd_drive_valid", test_devcmd_drive_valid},
	{"devcmd_drive_invalid", test_devcmd_drive_invalid},
	{"devcmd_unknown_type_allows_all", test_devcmd_unknown_type_allows_all},
	{"devcmd_shared_commands", test_devcmd_shared_commands},
	{"devcmd_inquiry_spelling", test_devcmd_inquiry_spelling},
	{"devcmd_prefix_matching_quirks", test_devcmd_prefix_matching_quirks},
	{NULL, NULL}
};
