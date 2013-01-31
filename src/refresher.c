/*
	belle-sip - SIP (RFC3261) library.
    Copyright (C) 2012  Belledonne Communications SARL

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "belle_sip_internal.h"
#include "belle-sip/refresher-helper.h"

struct belle_sip_refresher {
	belle_sip_object_t obj;
	belle_sip_refresher_listener_t listener;
	belle_sip_source_t* timer;
	belle_sip_client_transaction_t* transaction;
	int expires;
	unsigned int started;
	belle_sip_listener_callbacks_t listener_callbacks;
	belle_sip_listener_t *sip_listener;
	void* user_data;
};

static void process_dialog_terminated(void *user_ctx, const belle_sip_dialog_terminated_event_t *event){
	/*nop*/
}
static void process_io_error(void *user_ctx, const belle_sip_io_error_event_t *event){
	belle_sip_refresher_t* refresher=(belle_sip_refresher_t*)user_ctx;
	belle_sip_client_transaction_t*client_transaction;

	if (belle_sip_object_is_instance_of(BELLE_SIP_OBJECT(belle_sip_io_error_event_get_source(event)),BELLE_SIP_TYPE_ID(belle_sip_client_transaction_t))) {
		client_transaction=BELLE_SIP_CLIENT_TRANSACTION(belle_sip_io_error_event_get_source(event));
		if (refresher && (client_transaction !=refresher->transaction))
				return; /*not for me*/

		/*first stop timer if any*/
		belle_sip_refresher_stop(refresher);
		refresher->listener(refresher,refresher->user_data,503, "io error");
		return;
	} else {
		belle_sip_error("Refresher process_io_error not implemented yet for non transaction source");
	}
}

static int set_expires_from_trans(belle_sip_refresher_t* refresher);

static int timer_cb(void *user_data, unsigned int events) ;

static void schedule_timer(belle_sip_refresher_t* refresher) {
	if (refresher->expires>0) {
		if (refresher->timer){
			belle_sip_main_loop_remove_source(belle_sip_stack_get_main_loop(refresher->transaction->base.provider->stack),refresher->timer);
			belle_sip_object_unref(refresher->timer);
		}
		refresher->timer=belle_sip_timeout_source_new(timer_cb,refresher,refresher->expires*1000);
		belle_sip_object_set_name((belle_sip_object_t*)refresher->timer,"Refresher timeout");
		belle_sip_main_loop_add_source(belle_sip_stack_get_main_loop(refresher->transaction->base.provider->stack),refresher->timer);
	}
}

static void process_response_event(void *user_ctx, const belle_sip_response_event_t *event){
	belle_sip_client_transaction_t* client_transaction = belle_sip_response_event_get_client_transaction(event);
	belle_sip_response_t* response = belle_sip_response_event_get_response(event);
	int response_code = belle_sip_response_get_status_code(response);
	belle_sip_refresher_t* refresher=(belle_sip_refresher_t*)user_ctx;
	if (refresher && (client_transaction !=refresher->transaction))
		return; /*not for me*/

		/*handle authorization*/
		switch (response_code) {
		case 200: {
			/*great, success*/
			/*update expire if needed*/
			set_expires_from_trans(refresher);
			schedule_timer(refresher); /*re-arm timer*/
			break;
		}
		case 401:
		case 407:{
			belle_sip_refresher_refresh(refresher,refresher->expires); /*authorization is supposed to be available immediately*/
			return;
		}
		default:
			break;
		}
		refresher->listener(refresher,refresher->user_data,response_code, belle_sip_response_get_reason_phrase(response));

}
static void process_timeout(void *user_ctx, const belle_sip_timeout_event_t *event) {
	belle_sip_refresher_t* refresher=(belle_sip_refresher_t*)user_ctx;
	belle_sip_client_transaction_t*client_transaction =belle_sip_timeout_event_get_client_transaction(event);

	if (refresher && (client_transaction !=refresher->transaction))
				return; /*not for me*/

		/*first stop timer if any*/
	belle_sip_refresher_stop(refresher);
	refresher->listener(refresher,refresher->user_data,408, "timeout");
	return;
}
static void process_transaction_terminated(void *user_ctx, const belle_sip_transaction_terminated_event_t *event) {
	belle_sip_message("process_transaction_terminated Transaction terminated [%p]",event);
}

static void destroy(belle_sip_refresher_t *refresher){
	belle_sip_provider_remove_internal_sip_listener(refresher->transaction->base.provider,refresher->sip_listener);
	belle_sip_object_unref(refresher->transaction);
	belle_sip_object_unref(refresher->sip_listener);
}

BELLE_SIP_DECLARE_NO_IMPLEMENTED_INTERFACES(belle_sip_refresher_t);

BELLE_SIP_INSTANCIATE_VPTR(belle_sip_refresher_t, belle_sip_object_t,destroy, NULL, NULL,FALSE);

void belle_sip_refresher_set_listener(belle_sip_refresher_t* refresher, belle_sip_refresher_listener_t listener,void* user_pointer) {
	refresher->listener=listener;
	refresher->user_data=user_pointer;
}

int belle_sip_refresher_refresh(belle_sip_refresher_t* refresher,int expires) {
	belle_sip_request_t*old_request=belle_sip_transaction_get_request(BELLE_SIP_TRANSACTION(refresher->transaction));
	belle_sip_dialog_t* dialog = belle_sip_transaction_get_dialog(BELLE_SIP_TRANSACTION(refresher->transaction));
	belle_sip_client_transaction_t* client_transaction;
	belle_sip_request_t* request;
	belle_sip_header_expires_t* expires_header;
	belle_sip_uri_t* preset_route=refresher->transaction->preset_route;
	belle_sip_provider_t* prov=refresher->transaction->base.provider;
	belle_sip_header_contact_t* contact;
	/*first remove timer if any*/
	belle_sip_refresher_stop(refresher);

	if (!dialog) {
		/*create new request*/
		request=belle_sip_client_transaction_create_authenticated_request(refresher->transaction);
	} else if (dialog && belle_sip_dialog_get_state(dialog)==BELLE_SIP_DIALOG_CONFIRMED) {
		request=belle_sip_dialog_create_request_from(dialog,old_request);
		if (strcmp(belle_sip_request_get_method(request),"SUBSCRIBE")==0) {
			/*put expire header*/
			if (!(expires_header = belle_sip_message_get_header_by_type(request,belle_sip_header_expires_t))) {
				expires_header = belle_sip_header_expires_new();
				belle_sip_message_add_header(BELLE_SIP_MESSAGE(request),BELLE_SIP_HEADER(expires_header));
			}


		}
		belle_sip_provider_add_authorization(prov,request,NULL,NULL);
	} else {
		belle_sip_error("Unexpected dialog state [%s] for dialog [%p], cannot refresh [%s]"
				,belle_sip_dialog_state_to_string(belle_sip_dialog_get_state(dialog))
				,dialog
				,belle_sip_request_get_method(old_request));
		return -1;
	}
	/*update expires in any cases*/
	expires_header = belle_sip_message_get_header_by_type(request,belle_sip_header_expires_t);
	if (expires_header)
		belle_sip_header_expires_set_expires(expires_header,expires);
	contact=belle_sip_message_get_header_by_type(request,belle_sip_header_contact_t);
	if (belle_sip_header_contact_get_expires(contact)>=0)
		belle_sip_header_contact_set_expires(contact,expires);

	client_transaction = belle_sip_provider_create_client_transaction(prov,request);
	client_transaction->base.is_internal=1;
	belle_sip_transaction_set_application_data(BELLE_SIP_TRANSACTION(client_transaction),refresher);
	/*update reference transaction for next refresh*/
	belle_sip_object_unref(refresher->transaction);
	refresher->transaction=client_transaction;
	belle_sip_object_ref(refresher->transaction);

	if (belle_sip_client_transaction_send_request_to(client_transaction,preset_route)) {
		belle_sip_error("Cannot send refresh method [%s] for refresher [%p]"
				,belle_sip_request_get_method(old_request)
				,refresher);
		return -1;
	}
	return 0;
}


static int timer_cb(void *user_data, unsigned int events) {
	belle_sip_refresher_t* refresher = (belle_sip_refresher_t*)user_data;
	belle_sip_refresher_refresh(refresher,refresher->expires);
	return BELLE_SIP_STOP;
}

static belle_sip_header_contact_t* get_matching_contact(const belle_sip_transaction_t* transaction) {
	belle_sip_request_t*request=belle_sip_transaction_get_request(transaction);
	belle_sip_response_t*response=transaction->last_response;
	const belle_sip_list_t* contact_header_list;
	belle_sip_header_contact_t* unfixed_local_contact;
	belle_sip_header_contact_t* fixed_local_contact;
	char* tmp_string;
	char* tmp_string2;
	/*we assume, there is only one contact in request*/
	unfixed_local_contact= belle_sip_message_get_header_by_type(BELLE_SIP_MESSAGE(request),belle_sip_header_contact_t);
	fixed_local_contact= BELLE_SIP_HEADER_CONTACT(belle_sip_object_clone(BELLE_SIP_OBJECT(unfixed_local_contact)));

	/*first fix contact using received/rport*/
	belle_sip_response_fix_contact(response,fixed_local_contact);
	contact_header_list = belle_sip_message_get_headers(BELLE_SIP_MESSAGE(response),BELLE_SIP_CONTACT);

	if (contact_header_list) {
			contact_header_list = belle_sip_list_find_custom((belle_sip_list_t*)contact_header_list
																,(belle_sip_compare_func)belle_sip_header_contact_not_equals
																, (const void*)fixed_local_contact);
			if (!contact_header_list) {
				/*reset header list*/
				contact_header_list = belle_sip_message_get_headers(BELLE_SIP_MESSAGE(response),BELLE_SIP_CONTACT);
				contact_header_list = belle_sip_list_find_custom((belle_sip_list_t*)contact_header_list
																,(belle_sip_compare_func)belle_sip_header_contact_not_equals
																,unfixed_local_contact);
			}
			if (!contact_header_list) {
				tmp_string=belle_sip_object_to_string(BELLE_SIP_OBJECT(fixed_local_contact));
				tmp_string2=belle_sip_object_to_string(BELLE_SIP_OBJECT(unfixed_local_contact));
				belle_sip_error("No matching contact neither for [%s] nor [%s]", tmp_string, tmp_string2);
				belle_sip_free(tmp_string);
				belle_sip_free(tmp_string2);
				return NULL;
			} else {
				return BELLE_SIP_HEADER_CONTACT(contact_header_list->data);
			}
		} else {
		return NULL;
	}

}
static int set_expires_from_trans(belle_sip_refresher_t* refresher) {
	belle_sip_transaction_t* transaction = BELLE_SIP_TRANSACTION(refresher->transaction);
	belle_sip_response_t*response=transaction->last_response;
	belle_sip_request_t*request=belle_sip_transaction_get_request(transaction);
	belle_sip_header_expires_t* expires_header;
	belle_sip_header_contact_t* contact_header;

	refresher->expires=-1;
	
	if (strcmp("REGISTER",belle_sip_request_get_method(request))==0
			|| strcmp("SUBSCRIBE",belle_sip_request_get_method(request))==0) {

		/*An "expires" parameter on the "Contact" header has no semantics for
		*   SUBSCRIBE and is explicitly not equivalent to an "Expires" header in
		*  a SUBSCRIBE request or response.
		*/
		if (strcmp("REGISTER",belle_sip_request_get_method(request))==0
				&& (contact_header=get_matching_contact(transaction))!=NULL){
			refresher->expires=belle_sip_header_contact_get_expires(BELLE_SIP_HEADER_CONTACT(contact_header));
			/*great, we have an expire param from contact header*/
		}
		if (refresher->expires==-1){
			/*no contact with expire or not relevant, looking for Expires header*/
			if ((expires_header=(belle_sip_header_expires_t*)belle_sip_message_get_header(BELLE_SIP_MESSAGE(response),BELLE_SIP_EXPIRES))) {
				refresher->expires = belle_sip_header_expires_get_expires(expires_header);
			}
		}
		if (refresher->expires<0) {
			belle_sip_message("Neither Expires header nor corresponding Contact header found");
			refresher->expires=0;
			return 1;
		}

	} 	else if (strcmp("INVITE",belle_sip_request_get_method(request))==0) {
		belle_sip_fatal("Refresher does not support ERROR yet");
	} else {
		belle_sip_error("Refresher does not support [%s] yet",belle_sip_request_get_method(request));
		return 1;
	}
	return 0;
}


int belle_sip_refresher_start(belle_sip_refresher_t* refresher) {
	if(refresher->started) {
		belle_sip_warning("Refresher[%p] already started",refresher);
	} else {
		if (refresher->expires>0) {
			schedule_timer(refresher);
			belle_sip_message("Refresher [%p] started, next refresh in [%i] s",refresher,refresher->expires);
		}
	}
	return 0;
}

void belle_sip_refresher_stop(belle_sip_refresher_t* refresher) {
	if (refresher->timer){
		belle_sip_main_loop_remove_source(belle_sip_stack_get_main_loop(refresher->transaction->base.provider->stack), refresher->timer);
		belle_sip_object_unref(refresher->timer);
		refresher->timer=NULL;
	}
}
belle_sip_refresher_t* belle_sip_refresher_new(belle_sip_client_transaction_t* transaction) {
	belle_sip_refresher_t* refresher;
	if (belle_sip_transaction_get_state(BELLE_SIP_TRANSACTION(transaction)) != BELLE_SIP_TRANSACTION_COMPLETED) {
		belle_sip_error("Invalid state [%s] for transaction [%p], should be BELLE_SIP_TRANSACTION_COMPLETED"
					,belle_sip_transaction_state_to_string(belle_sip_transaction_get_state(BELLE_SIP_TRANSACTION(transaction)))
					,transaction);
		return NULL;
	}
	refresher = (belle_sip_refresher_t*)belle_sip_object_new(belle_sip_refresher_t);
	refresher->transaction=transaction;
	belle_sip_object_ref(transaction);
	refresher->listener_callbacks.process_response_event=process_response_event;
	refresher->listener_callbacks.process_timeout=process_timeout;
	refresher->listener_callbacks.process_io_error=process_io_error;
	refresher->listener_callbacks.process_dialog_terminated=process_dialog_terminated;
	refresher->listener_callbacks.process_transaction_terminated=process_transaction_terminated;;
	refresher->sip_listener=belle_sip_listener_create_from_callbacks(&(refresher->listener_callbacks),refresher);
	belle_sip_provider_add_internal_sip_listener(transaction->base.provider,refresher->sip_listener);
	if (set_expires_from_trans(refresher)){
		belle_sip_error("Unable to extract refresh value from transaction [%p]",transaction);
	}
	return refresher;
}
int belle_sip_refresher_get_expires(const belle_sip_refresher_t* refresher) {
	return refresher->expires;
}