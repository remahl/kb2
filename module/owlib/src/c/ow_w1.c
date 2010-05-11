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

#if OW_W1

#include "ow_w1.h"
#include "ow_connection.h"
#include "ow_codes.h"
#include "ow_counters.h"

struct toW1 {
	ASCII *command;
	ASCII lock[10];
	ASCII conditional[1];
	ASCII address[16];
	const BYTE *data;
	size_t length;
};

//static void byteprint( const BYTE * b, int size ) ;
static RESET_TYPE W1_reset(const struct parsedname *pn);
static enum search_status W1_next_both(struct device_search *ds, const struct parsedname *pn);
static GOOD_OR_BAD W1_sendback_data(const BYTE * data, BYTE * resp, const size_t len, const struct parsedname *pn);
static GOOD_OR_BAD W1_select_and_sendback(const BYTE * data, BYTE * resp, const size_t len, const struct parsedname *pn);
static void W1_setroutines(struct connection_in *in);
static void W1_close(struct connection_in *in);

static void W1_setroutines(struct connection_in *in)
{
	in->iroutines.detect = W1_detect;
	in->iroutines.reset = W1_reset;
	in->iroutines.next_both = W1_next_both;
	in->iroutines.PowerByte = NULL;
	//    in->iroutines.ProgramPulse = ;
	in->iroutines.select_and_sendback = W1_select_and_sendback;
	in->iroutines.sendback_data = W1_sendback_data;
	//    in->iroutines.sendback_bits = ;
	in->iroutines.select = NULL;
	in->iroutines.reconnect = NULL;
	in->iroutines.close = W1_close;
	// Directory obtained in a single gulp (W1_LIST_SLAVES)
	// Bundle transactions
	//
	in->iroutines.flags = ADAP_FLAG_dirgulp | ADAP_FLAG_bundle | ADAP_FLAG_dir_auto_reset;
	in->bundling_length = W1_FIFO_SIZE;	// arbitrary number
}

GOOD_OR_BAD W1_detect(struct connection_in *in)
{
	struct parsedname pn;

	FS_ParsedName_Placeholder(&pn);	// minimal parsename -- no destroy needed
	pn.selected_connection = in;
	LEVEL_CONNECT("start");

	/* Set up low-level routines */
	W1_setroutines(in);

	if ( pipe( in->connin.w1.netlink_pipe ) != 0 ) {
		ERROR_CONNECT("W1 pipe creation error");
		in->connin.w1.netlink_pipe[fd_pipe_read] = -1 ;
		in->connin.w1.netlink_pipe[fd_pipe_write] = -1 ;
		return gbBAD ;
	}

	if (in->name == NULL) {
		return gbBAD;
	}

	in->Adapter = adapter_w1;
	in->adapter_name = "w1";
	in->busmode = bus_w1;
	return gbGOOD;
}

/* Send blindly, no response expected */
static int w1_send_reset( const struct parsedname *pn )
{
    struct w1_netlink_msg w1m;
    struct w1_netlink_cmd w1c;

    memset(&w1m, 0, W1_W1M_LENGTH);
    w1m.type = W1_MASTER_CMD;
    w1m.id.mst.id = pn->selected_connection->connin.w1.id ;

    memset(&w1c, 0, W1_W1C_LENGTH);
    w1c.cmd = W1_CMD_RESET ;
    w1c.len = 0 ;

	LEVEL_DEBUG("Sending w1 reset message");
    return W1_send_msg( pn->selected_connection, &w1m, &w1c, NULL );
}

static RESET_TYPE W1_reset(const struct parsedname *pn)
{
	return W1_Process_Response( NULL, w1_send_reset(pn), NULL, pn ) == nrs_complete ? BUS_RESET_OK : BUS_RESET_ERROR ;
}

static int w1_send_search( BYTE search, const struct parsedname *pn )
{
	struct w1_netlink_msg w1m;
	struct w1_netlink_cmd w1c;

	memset(&w1m, 0, W1_W1M_LENGTH);
	w1m.type = W1_MASTER_CMD;
	w1m.id.mst.id = pn->selected_connection->connin.w1.id ;

	memset(&w1c, 0, W1_W1C_LENGTH);
	w1c.cmd = (search==_1W_CONDITIONAL_SEARCH_ROM) ? W1_CMD_ALARM_SEARCH : W1_CMD_SEARCH ;
	w1c.len = 0 ;

	LEVEL_DEBUG("Sending w1 search (list devices) message");
	return W1_send_msg( pn->selected_connection, &w1m, &w1c, NULL );
}

static void search_callback( struct netlink_parse * nlp, void * v, const struct parsedname * pn )
{
	int i ;
	struct dirblob *db = v ;
	(void) pn ;
	for ( i = 0 ; i < nlp->w1c->len ; i += 8 ) {
		DirblobAdd(&nlp->data[i], db);
	}
}

static enum search_status W1_next_both(struct device_search *ds, const struct parsedname *pn)
{
	struct dirblob *db = (ds->search == _1W_CONDITIONAL_SEARCH_ROM) ?
		&(pn->selected_connection->alarm) : &(pn->selected_connection->main);

	if (ds->LastDevice) {
		return search_done;
	}
	if (++(ds->index) == 0) {
		DirblobClear(db);
		if ( W1_Process_Response( search_callback, w1_send_search(ds->search,pn), db, pn ) != nrs_complete) {
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

static int w1_send_selecttouch( const BYTE * data, size_t size, const struct parsedname *pn )
{
	struct w1_netlink_msg w1m;
	struct w1_netlink_cmd w1c;

	memset(&w1m, 0, W1_W1M_LENGTH);
	w1m.type = W1_SLAVE_CMD;
	memcpy( w1m.id.id, pn->sn, 8) ;

	memset(&w1c, 0, W1_W1C_LENGTH);
	w1c.cmd = W1_CMD_TOUCH ;
	w1c.len = size ;

	LEVEL_DEBUG("Sending w1 select message for "SNformat,SNvar(pn->sn));
	return W1_send_msg( pn->selected_connection, &w1m, &w1c, data );
}

struct touch_struct {
	BYTE * resp ;
	size_t size ;
} ;

static void touch( struct netlink_parse * nlp, void * v, const struct parsedname * pn )
{
	struct touch_struct * ts = v ;
	(void) pn ;
	if ( nlp->data == NULL || ts->size != (size_t)nlp->data_size ) {
		return ;
	}
	memcpy( ts->resp, nlp->data, nlp->data_size ) ;
}

// Reset, select, and read/write data
static GOOD_OR_BAD W1_select_and_sendback(const BYTE * data, BYTE * resp, const size_t size, const struct parsedname *pn)
{
	struct touch_struct ts = { resp, size, } ;
	return W1_Process_Response( touch, w1_send_selecttouch(data,size,pn), &ts, pn)==nrs_complete ? gbGOOD : gbBAD ;
}

static int w1_send_touch( const BYTE * data, size_t size, const struct parsedname *pn )
{
	struct w1_netlink_msg w1m;
	struct w1_netlink_cmd w1c;

	memset(&w1m, 0, W1_W1M_LENGTH);
	w1m.type = W1_MASTER_CMD;
	w1m.id.mst.id = pn->selected_connection->connin.w1.id ;

	memset(&w1c, 0, W1_W1C_LENGTH);
	w1c.cmd = W1_CMD_TOUCH ;
	w1c.len = size ;

	LEVEL_DEBUG("Sending w1 send/receive data message for "SNformat,SNvar(pn->sn));
	return W1_send_msg( pn->selected_connection, &w1m, &w1c, data );
}

//  Send data and return response block
static GOOD_OR_BAD W1_sendback_data(const BYTE * data, BYTE * resp, const size_t size, const struct parsedname *pn)
{
	struct touch_struct ts = { resp, size, } ;
	return W1_Process_Response( touch, w1_send_touch(data,size,pn), &ts, pn)==nrs_complete ? gbGOOD : gbBAD ;
}

static void W1_close(struct connection_in *in)
{
	Test_and_Close_Pipe( in->connin.w1.netlink_pipe );
}

#endif							/* OW_W1 */
