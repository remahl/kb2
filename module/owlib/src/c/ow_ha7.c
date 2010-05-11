/*
$Id$
    OWFS -- One-Wire filesystem
    OWHTTPD -- One-Wire Web Server
    Written 2003 Paul H Alfille
	email: palfille@earthlink.net
	Released under the GPL
	See the header file: ow.h for full attribution
	1wire/iButton system from Dallas Semiconductor
*/

#include <config.h>
#include "owfs_config.h"
#include "ow.h"
#include "ow_counters.h"
#include "ow_connection.h"
#include "ow_codes.h"

#if OW_HA7

struct toHA7 {
	ASCII *command;
	ASCII lock[10];
	ASCII conditional[1];
	ASCII address[16];
	const BYTE *data;
	size_t length;
};

//static void byteprint( const BYTE * b, int size ) ;
static GOOD_OR_BAD HA7_write(FILE_DESCRIPTOR_OR_ERROR file_descriptor, const ASCII * msg, size_t size, struct connection_in *in);
static void toHA7init(struct toHA7 *ha7);
static void setHA7address(struct toHA7 *ha7, const BYTE * sn);
static GOOD_OR_BAD HA7_toHA7(FILE_DESCRIPTOR_OR_ERROR file_descriptor, const struct toHA7 *ha7, struct connection_in *in);
static GOOD_OR_BAD HA7_read(FILE_DESCRIPTOR_OR_ERROR file_descriptor, struct memblob *mb);
static RESET_TYPE HA7_reset(const struct parsedname *pn);
static enum search_status HA7_next_both(struct device_search *ds, const struct parsedname *pn);
static GOOD_OR_BAD HA7_sendback_data(const BYTE * data, BYTE * resp, const size_t len, const struct parsedname *pn);
static GOOD_OR_BAD HA7_select_and_sendback(const BYTE * data, BYTE * resp, const size_t len, const struct parsedname *pn);
static GOOD_OR_BAD HA7_sendback_block(const BYTE * data, BYTE * resp, const size_t size, int also_address, const struct parsedname *pn);
static GOOD_OR_BAD HA7_select(const struct parsedname *pn);
static void HA7_setroutines(struct connection_in *in);
static void HA7_close(struct connection_in *in);
static GOOD_OR_BAD HA7_directory(BYTE search, struct dirblob *db, const struct parsedname *pn);

static void HA7_setroutines(struct connection_in *in)
{
	in->iroutines.detect = HA7_detect;
	in->iroutines.reset = HA7_reset;
	in->iroutines.next_both = HA7_next_both;
	in->iroutines.PowerByte = NULL;
	//    in->iroutines.ProgramPulse = ;
	in->iroutines.select_and_sendback = HA7_select_and_sendback;
	in->iroutines.sendback_data = HA7_sendback_data;
	//    in->iroutines.sendback_bits = ;
	in->iroutines.select = HA7_select;
	in->iroutines.reconnect = NULL;
	in->iroutines.close = HA7_close;
	in->iroutines.flags = ADAP_FLAG_dirgulp | ADAP_FLAG_bundle | ADAP_FLAG_dir_auto_reset;
	in->bundling_length = HA7_FIFO_SIZE;	// arbitrary number
}

GOOD_OR_BAD HA7_detect(struct connection_in *in)
{
	struct parsedname pn;
	FILE_DESCRIPTOR_OR_ERROR file_descriptor;
	struct toHA7 ha7;

	FS_ParsedName_Placeholder(&pn);	// minimal parsename -- no destroy needed
	pn.selected_connection = in;
	LEVEL_CONNECT("start");

	/* Set up low-level routines */
	HA7_setroutines(in);

	in->connin.ha7.locked = 0;

	if (in->name == NULL) {
		return gbBAD;
	}

	/* Add the port if it isn't there already */
	if (strchr(in->name, ':') == NULL) {
		ASCII *temp = owrealloc(in->name, strlen(in->name) + 3);
		if (temp == NULL) {
			return gbBAD;
		}
		in->name = temp;
		strcat(in->name, ":80");
	}

	RETURN_BAD_IF_BAD(ClientAddr(in->name, in)) ;

	file_descriptor = ClientConnect(in) ;
	if ( FILE_DESCRIPTOR_NOT_VALID(file_descriptor) ) {
		return gbBAD;
	}

	in->Adapter = adapter_HA7NET;

	toHA7init(&ha7);
	ha7.command = "ReleaseLock";
	if (GOOD( HA7_toHA7(file_descriptor, &ha7, in)) ) {
		struct memblob mb;
		if ( GOOD( HA7_read(file_descriptor, &mb)) ) {
			in->adapter_name = "HA7Net";
			in->busmode = bus_ha7net;
			in->AnyDevices = anydevices_yes;
			MemblobClear(&mb);
			close(file_descriptor);
			return gbGOOD;
		}
	}
	close(file_descriptor);
	return gbBAD;
}

static RESET_TYPE HA7_reset(const struct parsedname *pn)
{
	struct memblob mb;
	FILE_DESCRIPTOR_OR_ERROR file_descriptor = ClientConnect(pn->selected_connection);
	RESET_TYPE ret = BUS_RESET_OK;
	struct toHA7 ha7;

	if ( FILE_DESCRIPTOR_NOT_VALID(file_descriptor) ) {
		return BUS_RESET_ERROR;
	}

	toHA7init(&ha7);
	ha7.command = "Reset";
	if ( BAD(HA7_toHA7(file_descriptor, &ha7, pn->selected_connection)) ) {
		LEVEL_DEBUG("Trouble sending reset command");
		ret = BUS_RESET_ERROR;
	} else if ( BAD(HA7_read(file_descriptor, &mb)) ) {
		LEVEL_DEBUG("Trouble with reset command response");
		ret = BUS_RESET_ERROR;
	}
	MemblobClear(&mb);
	close(file_descriptor);
	return ret;
}

static GOOD_OR_BAD HA7_directory(BYTE search, struct dirblob *db, const struct parsedname *pn)
{
	FILE_DESCRIPTOR_OR_ERROR file_descriptor;
	GOOD_OR_BAD ret = gbGOOD;
	struct toHA7 ha7;
	struct memblob mb;

	DirblobClear(db);
	file_descriptor = ClientConnect(pn->selected_connection) ;
	if ( FILE_DESCRIPTOR_NOT_VALID(file_descriptor) ) {
		db->troubled = 1;
		return gbBAD;
	}

	toHA7init(&ha7);
	ha7.command = "Search";
	if (search == _1W_CONDITIONAL_SEARCH_ROM) {
		ha7.conditional[0] = '1';
	}

	if ( BAD(HA7_toHA7(file_descriptor, &ha7, pn->selected_connection)) ) {
		ret = gbBAD;
	} else if ( BAD(HA7_read(file_descriptor, &mb)) ) {
		STAT_ADD1_BUS(e_bus_read_errors, pn->selected_connection);
		ret = gbBAD;
	} else {
		BYTE sn[SERIAL_NUMBER_SIZE];
		ASCII *p = (ASCII *) MemblobData(&mb);
		while ((p = strstr(p, "<INPUT CLASS=\"HA7Value\" NAME=\"Address_"))
			   && (p = strstr(p, "VALUE=\""))) {
			p += 7;
			if (strspn(p, "0123456789ABCDEF") < 16) {
				ret = gbBAD;
				break;
			}
			sn[7] = string2num(&p[0]);
			sn[6] = string2num(&p[2]);
			sn[5] = string2num(&p[4]);
			sn[4] = string2num(&p[6]);
			sn[3] = string2num(&p[8]);
			sn[2] = string2num(&p[10]);
			sn[1] = string2num(&p[12]);
			sn[0] = string2num(&p[14]);
			if (CRC8(sn, SERIAL_NUMBER_SIZE)) {
				ret = gbBAD;
				break;
			}
			DirblobAdd(sn, db);
		}
		MemblobClear(&mb);
	}
	close(file_descriptor);
	return ret;
}

static enum search_status HA7_next_both(struct device_search *ds, const struct parsedname *pn)
{
	struct dirblob *db = (ds->search == _1W_CONDITIONAL_SEARCH_ROM) ?
		&(pn->selected_connection->alarm) : &(pn->selected_connection->main);

	if (pn->selected_connection->AnyDevices == anydevices_no) {
		ds->LastDevice = 1;
	}
	if (ds->LastDevice) {
		return search_done;
	}

	if (++(ds->index) == 0) {
		if ( BAD(HA7_directory(ds->search, db, pn)) ) {
			return search_error;
		}
	}
	switch ( DirblobGet(ds->index, ds->sn, db) ) {
		case 0:
			LEVEL_DEBUG("SN found: " SNformat "", SNvar(ds->sn));
			return search_good;
		case -ENODEV:
		default:
			ds->LastDevice = 1;
			LEVEL_DEBUG("SN finished");
			return search_done;
	}
}

#define HA7_READ_BUFFER_LENGTH 2000

static GOOD_OR_BAD HA7_read(FILE_DESCRIPTOR_OR_ERROR file_descriptor, struct memblob *mb)
{
	ASCII readin_area[HA7_READ_BUFFER_LENGTH + 1];
	ASCII *start;
	size_t read_size;
	struct timeval tvnet = { Globals.timeout_ha7, 0, };

	MemblobInit(mb, HA7_READ_BUFFER_LENGTH);

	// Read first block of data from HA7
	tcp_read(file_descriptor, readin_area, HA7_READ_BUFFER_LENGTH, &tvnet, &read_size) ;
	if ( read_size == 0) {
		LEVEL_CONNECT("(ethernet) error = %d", read_size);
		//write(1, readin_area, read_size);
		return gbBAD;
	}
	// make sure null terminated (allocated extra byte in readin_area to always have room)
	readin_area[read_size] = '\0';

	// Look for happy response
	if (strncmp("HTTP/1.1 200 OK", readin_area, 15)) {	//Bad HTTP return code
		ASCII *p = strchr(&readin_area[15], '\n');
		if (p == NULL) {
			p = &readin_area[15 + 32];
		}
		LEVEL_DATA("response problem:%.*s", p - readin_area - 15, &readin_area[15]);
		return gbBAD;
	}
	// Look for "<body>"
	if ((start = strstr(readin_area, "<body>")) == NULL) {
		LEVEL_DATA("esponse: No HTTP body to parse");
		MemblobClear(mb);
		return gbBAD;
	}
	// HTML body found, dump header
	if (MemblobAdd((BYTE *) start, read_size - (start - readin_area), mb)) {
		MemblobClear(mb);
		return gbBAD;
	}
	// loop through reading in HA7_READ_BUFFER_LENGTH blocks
	while (read_size == HA7_READ_BUFFER_LENGTH) {	// full read, so presume more waiting
		tcp_read(file_descriptor, readin_area, HA7_READ_BUFFER_LENGTH, &tvnet, &read_size) ;
		if (read_size == 0) {
			LEVEL_DATA("Couldn't get rest of HA7 data (err=%d)", read_size);
			MemblobClear(mb);
			return gbBAD;
		} else if (MemblobAdd((BYTE *) readin_area, read_size, mb)) {
			MemblobClear(mb);
			return gbBAD;
		}
	}

	// Add trailing null
	if (MemblobAdd((BYTE *) "", 1, mb)) {
		MemblobClear(mb);
		return gbBAD;
	}
	//printf("READ FROM HA7:\n%s\n",MemblobData(mb));
	return gbGOOD;
}

static GOOD_OR_BAD HA7_write(FILE_DESCRIPTOR_OR_ERROR file_descriptor, const ASCII * msg, size_t length, struct connection_in *in)
{
	ssize_t r, sl = length;
	ssize_t size = sl;
	while (sl > 0) {
		r = write(file_descriptor, &msg[size - sl], sl);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			ERROR_CONNECT("Trouble writing data to HA7: %s", SAFESTRING(in->name));
			break;
		}
		sl -= r;
	}
	gettimeofday(&(in->bus_write_time), NULL);
	if (sl > 0) {
		STAT_ADD1_BUS(e_bus_write_errors, in);
		return gbBAD;
	}
	return gbGOOD;
}

static GOOD_OR_BAD HA7_toHA7(FILE_DESCRIPTOR_OR_ERROR file_descriptor, const struct toHA7 *ha7, struct connection_in *in)
{
	int first = 1;
	int probable_length;
	GOOD_OR_BAD ret ;
	char *full_command;
	LEVEL_DEBUG
		("To HA7 command=%s address=%.16s conditional=%.1s lock=%.10s",
		 SAFESTRING(ha7->command), SAFESTRING(ha7->address), SAFESTRING(ha7->conditional), SAFESTRING(ha7->lock));

	if (ha7->command == NULL) {
		return gbBAD;
	}

	probable_length = 11 + strlen(ha7->command) + 5 + ((ha7->address[0]) ? 1 + 8 + 16 : 0)
		+ ((ha7->conditional[0]) ? 1 + 12 + 1 : 0)
		+ ((ha7->data) ? 1 + 5 + ha7->length * 2 : 0)
		+ ((ha7->lock[0]) ? 1 + 7 + 10 : 0)
		+ 11 + 1;

	full_command = owmalloc(probable_length);
	if (full_command == NULL) {
		return gbBAD;
	}
	memset(full_command, 0, probable_length);

	strcpy(full_command, "GET /1Wire/");
	strcat(full_command, ha7->command);
	strcat(full_command, ".html");

	if (ha7->address[0]) {
		strcat(full_command, first ? "?" : "&");
		strcat(full_command, "Address=");
		strcat(full_command, ha7->address);
		first = 0;
	}

	if (ha7->conditional[0]) {
		strcat(full_command, first ? "?" : "&");
		strcat(full_command, "Conditional=");
		strcat(full_command, ha7->conditional);
		first = 0;
	}

	if (ha7->data) {
		strcat(full_command, first ? "?" : "&");
		strcat(full_command, "Data=");
		bytes2string(&full_command[strlen(full_command)], ha7->data, ha7->length);
	}

	if (ha7->lock[0]) {
		strcat(full_command, first ? "?" : "&");
		strcat(full_command, "LockID=");
		strcat(full_command, ha7->lock);
		first = 0;
	}

	strcat(full_command, " HTTP/1.0\n\n");

	LEVEL_DEBUG("To HA7 %s", full_command);

	ret = HA7_write(file_descriptor, full_command, probable_length, in) ;
	owfree(full_command);
	return ret;
}

// Reset, select, and read/write data
/* return 0=good
   sendout_data, readin
 */
static GOOD_OR_BAD HA7_select_and_sendback(const BYTE * data, BYTE * resp, const size_t size, const struct parsedname *pn)
{
	size_t location = 0;
	int also_address = 1;

	while (location < size) {
		size_t block = size - location;
		if (block > 32) {
			block = 32;
		}
		// Don't add address (that's the "0")
		RETURN_BAD_IF_BAD(HA7_sendback_block(&data[location], &resp[location], block, also_address, pn)) ;
		location += block;
		also_address = 0;		//for subsequent blocks
	}
	return gbGOOD ;
}

//  Send data and return response block
/* return 0=good
   sendout_data, readin
 */
static GOOD_OR_BAD HA7_sendback_data(const BYTE * data, BYTE * resp, const size_t size, const struct parsedname *pn)
{
	size_t location = 0;

	while (location < size) {
		size_t block = size - location;
		if (block > 32) {
			block = 32;
		}
		// Don't add address (that's the "0")
		RETURN_BAD_IF_BAD(HA7_sendback_block(&data[location], &resp[location], block, 0, pn)) ;
		location += block;
	}
	return gbGOOD;
}

// HA7 only allows WriteBlock of 32 bytes
// This routine assumes that larger writes have already been broken up
static GOOD_OR_BAD HA7_sendback_block(const BYTE * data, BYTE * resp, const size_t size, int also_address, const struct parsedname *pn)
{
	FILE_DESCRIPTOR_OR_ERROR file_descriptor;
	struct memblob mb;
	struct toHA7 ha7;
	GOOD_OR_BAD ret = gbBAD;

	file_descriptor = ClientConnect(pn->selected_connection) ;
	if ( FILE_DESCRIPTOR_NOT_VALID(file_descriptor) ) {
		return gbBAD;
	}

	toHA7init(&ha7);
	ha7.command = "WriteBlock";
	ha7.data = data;
	ha7.length = size;
	if (also_address) {
		setHA7address(&ha7, pn->sn);
	}

	if ( GOOD( HA7_toHA7(file_descriptor, &ha7, pn->selected_connection)) ) {
		if ( GOOD( HA7_read(file_descriptor, &mb)) ) {
			ASCII *p = (ASCII *) MemblobData(&mb);
			if ((p = strstr(p, "<INPUT TYPE=\"TEXT\" NAME=\"ResultData_0\""))
				&& (p = strstr(p, "VALUE=\""))) {
				p += 7;
				LEVEL_DEBUG("HA7_sendback_data received(%d): %.*s", size * 2, size * 2, p);
				if (strspn(p, "0123456789ABCDEF") >= size << 1) {
					string2bytes(p, resp, size);
					ret = gbGOOD;
				}
			}
			MemblobClear(&mb);
		} else {
			STAT_ADD1_BUS(e_bus_read_errors, pn->selected_connection);
		}
	}
	close(file_descriptor);
	return gbGOOD;
}

static void setHA7address(struct toHA7 *ha7, const BYTE * sn)
{
	num2string(&(ha7->address[0]), sn[7]);
	num2string(&(ha7->address[2]), sn[6]);
	num2string(&(ha7->address[4]), sn[5]);
	num2string(&(ha7->address[6]), sn[4]);
	num2string(&(ha7->address[8]), sn[3]);
	num2string(&(ha7->address[10]), sn[2]);
	num2string(&(ha7->address[12]), sn[1]);
	num2string(&(ha7->address[14]), sn[0]);
}

static GOOD_OR_BAD HA7_select(const struct parsedname *pn)
{
	GOOD_OR_BAD ret = gbBAD;

	if (pn->selected_device) {
		FILE_DESCRIPTOR_OR_ERROR file_descriptor = ClientConnect(pn->selected_connection);

		if ( FILE_DESCRIPTOR_VALID(file_descriptor) ) {
			struct toHA7 ha7;
			toHA7init(&ha7);
			ha7.command = "AddressDevice";
			setHA7address(&ha7, pn->sn);
			if ( GOOD( HA7_toHA7(file_descriptor, &ha7, pn->selected_connection)) ) {
				struct memblob mb;
				if ( GOOD( HA7_read(file_descriptor, &mb)) ) {
					MemblobClear(&mb);
					ret = gbGOOD;
				}
			}
			close(file_descriptor);
		}
	} else {
		return HA7_reset(pn)==BUS_RESET_OK ? gbGOOD : gbBAD ;
	}
	return ret;
}

static void HA7_close(struct connection_in *in)
{
	FreeClientAddr(in);
}

static void toHA7init(struct toHA7 *ha7)
{
	memset(ha7, 0, sizeof(struct toHA7));
}

#endif							/* OW_HA7 */
