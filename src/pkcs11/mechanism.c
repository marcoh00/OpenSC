/*
 * Generic handling of PKCS11 mechanisms
 *
 * Copyright (C) 2002 Olaf Kirch <okir@suse.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "sc-pkcs11.h"

/* Also used for verification data */
struct hash_signature_info {
	CK_MECHANISM_TYPE	mech;
	CK_MECHANISM_TYPE	hash_mech;
	CK_MECHANISM_TYPE	sign_mech;
	sc_pkcs11_mechanism_type_t *hash_type;
};

/* Also used for verification and decryption data */
struct operation_data {
	struct sc_pkcs11_object *key;
	struct hash_signature_info *info;
	sc_pkcs11_operation_t *md;
	CK_BYTE			*buffer;
	unsigned int	buffer_len;
};

static struct operation_data *
new_operation_data()
{
	return calloc(1, sizeof(struct operation_data));
}

static void
operation_data_release(struct operation_data *data)
{
	if (!data)
		return;
	sc_pkcs11_release_operation(&data->md);
	sc_mem_secure_clear_free(data->buffer, data->buffer_len);
	free(data);
}

static CK_RV
signature_data_buffer_append(struct operation_data *data,
		const CK_BYTE *in, CK_ULONG in_len)
{
	if (!data)
		return CKR_ARGUMENTS_BAD;
	if (in_len == 0)
		return CKR_OK;

	unsigned int new_len = data->buffer_len + in_len;
	CK_BYTE *new_buffer = sc_mem_secure_alloc(new_len);
	if (!new_buffer)
		return CKR_HOST_MEMORY;

	if (data->buffer_len != 0)
		memcpy(new_buffer, data->buffer, data->buffer_len);
	memcpy(new_buffer+data->buffer_len, in, in_len);

	sc_mem_secure_clear_free(data->buffer, data->buffer_len);
	data->buffer = new_buffer;
	data->buffer_len = new_len;
	return CKR_OK;
}

void _update_mech_info(CK_MECHANISM_INFO_PTR mech_info, CK_MECHANISM_INFO_PTR new_mech_info) {
	if (new_mech_info->ulMaxKeySize > mech_info->ulMaxKeySize)
		mech_info->ulMaxKeySize = new_mech_info->ulMaxKeySize;
	if (new_mech_info->ulMinKeySize < mech_info->ulMinKeySize)
		mech_info->ulMinKeySize = new_mech_info->ulMinKeySize;
	mech_info->flags |= new_mech_info->flags;
}

/*
 * Copy a mechanism
 */
static CK_RV
sc_pkcs11_copy_mechanism(sc_pkcs11_mechanism_type_t *mt,
				sc_pkcs11_mechanism_type_t **new_mt)
{
	CK_RV rv;

	*new_mt = calloc(1, sizeof(sc_pkcs11_mechanism_type_t));
	if (!(*new_mt))
		return CKR_HOST_MEMORY;
	
	memcpy(*new_mt, mt, sizeof(sc_pkcs11_mechanism_type_t));
	/* mech_data needs specific function for making copy*/
	if (mt->copy_mech_data
		&& (rv = mt->copy_mech_data(mt->mech_data, (void **) &(*new_mt)->mech_data)) != CKR_OK) {
		free(*new_mt);
		return rv;
	}
	
	return CKR_OK;
}

/*
 * Register a mechanism
 * Check whether the mechanism is already in p11card,
 * if not, make a copy of the given mechanism and store it
 * in p11card.
 */
CK_RV
sc_pkcs11_register_mechanism(struct sc_pkcs11_card *p11card,
				sc_pkcs11_mechanism_type_t *mt, sc_pkcs11_mechanism_type_t **result_mt)
{
	sc_pkcs11_mechanism_type_t *existing_mt;
	sc_pkcs11_mechanism_type_t *copy_mt = NULL;
	sc_pkcs11_mechanism_type_t **p;
	int i;
	CK_RV rv;

	if (mt == NULL)
		return CKR_HOST_MEMORY;

	if ((existing_mt = sc_pkcs11_find_mechanism(p11card, mt->mech, mt->mech_info.flags))) {
		for (i = 0; i < MAX_KEY_TYPES; i++) {
			if (existing_mt->key_types[i] == mt->key_types[0]) {
				/* Mechanism already registered with the same key_type,
				 * just update it's info */
				_update_mech_info(&existing_mt->mech_info, &mt->mech_info);
				return CKR_OK;
			}
			if (existing_mt->key_types[i] < 0) {
				/* There is a free slot for new key type, let's add it and
				 * update mechanism info */
				_update_mech_info(&existing_mt->mech_info, &mt->mech_info);
				/* XXX Should be changed to loop over mt->key_types, if
				 * we allow to register mechanism with multiple key types
				 * in one operation */
				existing_mt->key_types[i] = mt->key_types[0];
				if (i + 1 < MAX_KEY_TYPES) {
					existing_mt->key_types[i + 1] = -1;
				}
				return CKR_OK;
			}
		}
		sc_log(p11card->card->ctx, "Too many key types in mechanism 0x%lx, more than %d", mt->mech, MAX_KEY_TYPES);
		return CKR_BUFFER_TOO_SMALL;
	}

	p = (sc_pkcs11_mechanism_type_t **) realloc(p11card->mechanisms,
			(p11card->nmechanisms + 2) * sizeof(*p));
	if (p == NULL)
		return CKR_HOST_MEMORY;
	if ((rv = sc_pkcs11_copy_mechanism(mt, &copy_mt)) != CKR_OK) {
		free(p);
		return rv;
	}
	p11card->mechanisms = p;
	p[p11card->nmechanisms++] = copy_mt;
	p[p11card->nmechanisms] = NULL;
	/* Return registered mechanism for further use */
	if (result_mt)
		*result_mt = copy_mt;
	return CKR_OK;
}


CK_RV
_validate_key_type(sc_pkcs11_mechanism_type_t *mech, CK_KEY_TYPE key_type) {
	int i;
	for (i = 0; i < MAX_KEY_TYPES; i++) {
		if (mech->key_types[i] < 0)
			break;
		if ((CK_KEY_TYPE)mech->key_types[i] == key_type)
			return CKR_OK;
	}
	return CKR_KEY_TYPE_INCONSISTENT;
}

/*
 * Look up a mechanism
 */
sc_pkcs11_mechanism_type_t *
sc_pkcs11_find_mechanism(struct sc_pkcs11_card *p11card, CK_MECHANISM_TYPE mech, unsigned int flags)
{
	sc_pkcs11_mechanism_type_t *mt;
	unsigned int n;

	for (n = 0; n < p11card->nmechanisms; n++) {
		mt = p11card->mechanisms[n];
		if (mt && mt->mech == mech && ((mt->mech_info.flags & flags) == flags))
			return mt;
	}
	return NULL;
}

/*
 * Query mechanisms.
 * All of this is greatly simplified by having the framework
 * register all supported mechanisms at initialization
 * time.
 */
CK_RV
sc_pkcs11_get_mechanism_list(struct sc_pkcs11_card *p11card,
				CK_MECHANISM_TYPE_PTR pList,
				CK_ULONG_PTR pulCount)
{
	sc_pkcs11_mechanism_type_t *mt;
	unsigned int n, count = 0;
	CK_RV rv;

	if (!p11card)
		return CKR_TOKEN_NOT_PRESENT;

	for (n = 0; n < p11card->nmechanisms; n++) {
		if (!(mt = p11card->mechanisms[n]))
			continue;
		if (pList && count < *pulCount)
			pList[count] = mt->mech;
		count++;
	}

	rv = CKR_OK;
	if (pList && count > *pulCount)
		rv = CKR_BUFFER_TOO_SMALL;
	*pulCount = count;
	return rv;
}

CK_RV
sc_pkcs11_get_mechanism_info(struct sc_pkcs11_card *p11card,
			CK_MECHANISM_TYPE mechanism,
			CK_MECHANISM_INFO_PTR pInfo)
{
	sc_pkcs11_mechanism_type_t *mt;

	if (!(mt = sc_pkcs11_find_mechanism(p11card, mechanism, 0)))
		return CKR_MECHANISM_INVALID;
	memcpy(pInfo, &mt->mech_info, sizeof(*pInfo));
	return CKR_OK;
}

/*
 * Create/destroy operation handle
 */
sc_pkcs11_operation_t *
sc_pkcs11_new_operation(sc_pkcs11_session_t *session,
			sc_pkcs11_mechanism_type_t *type)
{
	sc_pkcs11_operation_t *res;

	res = calloc(1, type->obj_size);
	if (res) {
		res->session = session;
		res->type = type;
	}
	return res;
}

void
sc_pkcs11_release_operation(sc_pkcs11_operation_t **ptr)
{
	sc_pkcs11_operation_t *operation = *ptr;

	if (!operation)
		return;
	if (operation->type && operation->type->release)
		operation->type->release(operation);
	memset(operation, 0, sizeof(*operation));
	free(operation);
	*ptr = NULL;
}

CK_RV
sc_pkcs11_md_init(struct sc_pkcs11_session *session,
			CK_MECHANISM_PTR pMechanism)
{
	struct sc_pkcs11_card *p11card;
	sc_pkcs11_operation_t *operation;
	sc_pkcs11_mechanism_type_t *mt;
	CK_RV rv;

	LOG_FUNC_CALLED(context);
	if (!session || !session->slot || !(p11card = session->slot->p11card))
		LOG_FUNC_RETURN(context, CKR_ARGUMENTS_BAD);

	/* See if we support this mechanism type */
	mt = sc_pkcs11_find_mechanism(p11card, pMechanism->mechanism, CKF_DIGEST);
	if (mt == NULL)
		LOG_FUNC_RETURN(context, CKR_MECHANISM_INVALID);

	rv = session_start_operation(session, SC_PKCS11_OPERATION_DIGEST, mt, &operation);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	memcpy(&operation->mechanism, pMechanism, sizeof(CK_MECHANISM));

	rv = mt->md_init(operation);

	if (rv != CKR_OK)
		session_stop_operation(session, SC_PKCS11_OPERATION_DIGEST);

	LOG_FUNC_RETURN(context, (int) rv);
}

CK_RV
sc_pkcs11_md_update(struct sc_pkcs11_session *session,
			CK_BYTE_PTR pData, CK_ULONG ulDataLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_DIGEST, &op);
	if (rv != CKR_OK)
		goto done;

	rv = op->type->md_update(op, pData, ulDataLen);

done:
	if (rv != CKR_OK)
		session_stop_operation(session, SC_PKCS11_OPERATION_DIGEST);

	LOG_FUNC_RETURN(context, (int) rv);
}

CK_RV
sc_pkcs11_md_final(struct sc_pkcs11_session *session,
			CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_DIGEST, &op);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	/* This is a request for the digest length */
	if (pData == NULL)
		*pulDataLen = 0;

	rv = op->type->md_final(op, pData, pulDataLen);
	if (rv == CKR_BUFFER_TOO_SMALL)
		LOG_FUNC_RETURN(context,  pData == NULL ? CKR_OK : CKR_BUFFER_TOO_SMALL);

	session_stop_operation(session, SC_PKCS11_OPERATION_DIGEST);
	LOG_FUNC_RETURN(context, (int) rv);
}

/*
 * Initialize a signing context. When we get here, we know
 * the key object is capable of signing _something_
 */
CK_RV
sc_pkcs11_sign_init(struct sc_pkcs11_session *session, CK_MECHANISM_PTR pMechanism,
		    struct sc_pkcs11_object *key, CK_KEY_TYPE key_type)
{
	struct sc_pkcs11_card *p11card;
	sc_pkcs11_operation_t *operation;
	sc_pkcs11_mechanism_type_t *mt;
	CK_RV rv;

	LOG_FUNC_CALLED(context);
	if (!session || !session->slot || !(p11card = session->slot->p11card))
		LOG_FUNC_RETURN(context, CKR_ARGUMENTS_BAD);

	/* See if we support this mechanism type */
	sc_log(context, "mechanism 0x%lX, key-type 0x%lX",
	       pMechanism->mechanism, key_type);
	mt = sc_pkcs11_find_mechanism(p11card, pMechanism->mechanism, CKF_SIGN);
	if (mt == NULL)
		LOG_FUNC_RETURN(context, CKR_MECHANISM_INVALID);

	/* See if compatible with key type */
	rv = _validate_key_type(mt, key_type);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	if (pMechanism->pParameter &&
	    pMechanism->ulParameterLen > sizeof(operation->mechanism_params))
		LOG_FUNC_RETURN(context, CKR_ARGUMENTS_BAD);

	rv = session_start_operation(session, SC_PKCS11_OPERATION_SIGN, mt, &operation);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	memcpy(&operation->mechanism, pMechanism, sizeof(CK_MECHANISM));
	if (pMechanism->pParameter) {
		memcpy(&operation->mechanism_params, pMechanism->pParameter,
		       pMechanism->ulParameterLen);
		operation->mechanism.pParameter = &operation->mechanism_params;
	}
	rv = mt->sign_init(operation, key);
	if (rv != CKR_OK)
		session_stop_operation(session, SC_PKCS11_OPERATION_SIGN);

	LOG_FUNC_RETURN(context, (int) rv);
}

CK_RV
sc_pkcs11_sign_update(struct sc_pkcs11_session *session,
		      CK_BYTE_PTR pData, CK_ULONG ulDataLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	LOG_FUNC_CALLED(context);
	rv = session_get_operation(session, SC_PKCS11_OPERATION_SIGN, &op);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	if (op->type->sign_update == NULL) {
		rv = CKR_KEY_TYPE_INCONSISTENT;
		goto done;
	}

	rv = op->type->sign_update(op, pData, ulDataLen);

done:
	if (rv != CKR_OK)
		session_stop_operation(session, SC_PKCS11_OPERATION_SIGN);

	LOG_FUNC_RETURN(context, (int) rv);
}

CK_RV
sc_pkcs11_sign_final(struct sc_pkcs11_session *session,
		     CK_BYTE_PTR pSignature, CK_ULONG_PTR pulSignatureLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	LOG_FUNC_CALLED(context);
	rv = session_get_operation(session, SC_PKCS11_OPERATION_SIGN, &op);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	/* Bail out for signature mechanisms that don't do hashing */
	if (op->type->sign_final == NULL) {
		rv = CKR_KEY_TYPE_INCONSISTENT;
		goto done;
	}

	rv = op->type->sign_final(op, pSignature, pulSignatureLen);

done:
	if (rv != CKR_BUFFER_TOO_SMALL && pSignature != NULL)
		session_stop_operation(session, SC_PKCS11_OPERATION_SIGN);

	LOG_FUNC_RETURN(context, (int) rv);
}

CK_RV
sc_pkcs11_sign_size(struct sc_pkcs11_session *session, CK_ULONG_PTR pLength)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_SIGN, &op);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	/* Bail out for signature mechanisms that don't do hashing */
	if (op->type->sign_size == NULL) {
		rv = CKR_KEY_TYPE_INCONSISTENT;
		goto done;
	}

	rv = op->type->sign_size(op, pLength);

done:
	if (rv != CKR_OK)
		session_stop_operation(session, SC_PKCS11_OPERATION_SIGN);

	LOG_FUNC_RETURN(context, (int) rv);
}

/*
 * Initialize a signature operation
 */
static CK_RV
sc_pkcs11_signature_init(sc_pkcs11_operation_t *operation,
		struct sc_pkcs11_object *key)
{
	struct hash_signature_info *info;
	struct operation_data *data;
	CK_RV rv;
	int can_do_it = 0;

	LOG_FUNC_CALLED(context);
	if (!(data = new_operation_data()))
		LOG_FUNC_RETURN(context, CKR_HOST_MEMORY);
	data->info = NULL;
	data->key = key;

	if (key->ops->can_do)   {
		rv = key->ops->can_do(operation->session, key, operation->type->mech, CKF_SIGN);
		if (rv == CKR_OK)   {
			/* Mechanism recognised and can be performed by pkcs#15 card */
			can_do_it = 1;
		}
		else if (rv == CKR_FUNCTION_NOT_SUPPORTED)   {
			/* Mechanism not recognised by pkcs#15 card */
			can_do_it = 0;
		}
		else  {
			/* Mechanism recognised but cannot be performed by pkcs#15 card, or some general error. */
			operation_data_release(data);
			LOG_FUNC_RETURN(context, (int) rv);
		}
	}

	/* Validate the mechanism parameters */
	if (key->ops->init_params) {
		rv = key->ops->init_params(operation->session, &operation->mechanism);
		if (rv != CKR_OK) {
			/* Probably bad arguments */
			operation_data_release(data);
			LOG_FUNC_RETURN(context, (int) rv);
		}
	}

	/* If this is a signature with hash operation,
	 * and card cannot perform itself signature with hash operation,
	 * set up the hash operation */
	info = (struct hash_signature_info *)operation->type->mech_data;
	if (info != NULL && !can_do_it) {
		/* Initialize hash operation */

		data->md = sc_pkcs11_new_operation(operation->session, info->hash_type);
		if (data->md == NULL)
			rv = CKR_HOST_MEMORY;
		else
			rv = info->hash_type->md_init(data->md);
		if (rv != CKR_OK) {
			sc_pkcs11_release_operation(&data->md);
			operation_data_release(data);
			LOG_FUNC_RETURN(context, (int) rv);
		}
		data->info = info;
	}

	operation->priv_data = data;
	LOG_FUNC_RETURN(context, CKR_OK);
}

static CK_RV
sc_pkcs11_signature_update(sc_pkcs11_operation_t *operation,
		CK_BYTE_PTR pPart, CK_ULONG ulPartLen)
{
	struct operation_data *data;
	CK_RV rv;

	LOG_FUNC_CALLED(context);
	sc_log(context, "data part length %li", ulPartLen);
	data = (struct operation_data *)operation->priv_data;
	if (data->md) {
		rv = data->md->type->md_update(data->md, pPart, ulPartLen);
		LOG_FUNC_RETURN(context, (int) rv);
	}

	/* This signature mechanism operates on the raw data */
	rv = signature_data_buffer_append(data, pPart, ulPartLen);
	LOG_FUNC_RETURN(context, (int) rv);
}

static CK_RV
sc_pkcs11_signature_final(sc_pkcs11_operation_t *operation,
		CK_BYTE_PTR pSignature, CK_ULONG_PTR pulSignatureLen)
{
	struct operation_data *data;
	CK_RV rv;

	LOG_FUNC_CALLED(context);
	data = (struct operation_data *)operation->priv_data;
	if (data->md) {
		sc_pkcs11_operation_t	*md = data->md;
		CK_BYTE hash[64];
		CK_ULONG len = sizeof(hash);

		rv = md->type->md_final(md, hash, &len);
		if (rv == CKR_BUFFER_TOO_SMALL)
			rv = CKR_FUNCTION_FAILED;
		if (rv != CKR_OK)
			LOG_FUNC_RETURN(context, (int) rv);
		rv = signature_data_buffer_append(data, hash, len);
		if (rv != CKR_OK)
			LOG_FUNC_RETURN(context, (int) rv);
	}

	rv = data->key->ops->sign(operation->session, data->key, &operation->mechanism,
			data->buffer, data->buffer_len, pSignature, pulSignatureLen);
	LOG_FUNC_RETURN(context, (int) rv);
}

static CK_RV
sc_pkcs11_signature_size(sc_pkcs11_operation_t *operation, CK_ULONG_PTR pLength)
{
	struct sc_pkcs11_object *key;
	CK_ATTRIBUTE attr = { CKA_MODULUS_BITS, pLength, sizeof(*pLength) };
	CK_KEY_TYPE key_type;
	CK_ATTRIBUTE attr_key_type = { CKA_KEY_TYPE, &key_type, sizeof(key_type) };
	CK_RV rv;

	key = ((struct operation_data *)operation->priv_data)->key;
	/*
	 * EC and GOSTR do not have CKA_MODULUS_BITS attribute.
	 * But other code in framework treats them as if they do.
	 * So should do switch(key_type)
	 * and then get what ever attributes are needed.
	 */
	rv = key->ops->get_attribute(operation->session, key, &attr_key_type);
	if (rv == CKR_OK) {
		switch(key_type) {
			case CKK_RSA:
				rv = key->ops->get_attribute(operation->session, key, &attr);
				/* convert bits to bytes */
				if (rv == CKR_OK)
					*pLength = (*pLength + 7) / 8;
				break;
			case CKK_EC:
			case CKK_EC_EDWARDS:
			case CKK_EC_MONTGOMERY:
				/* TODO: -DEE we should use something other then CKA_MODULUS_BITS... */
				rv = key->ops->get_attribute(operation->session, key, &attr);
				if (rv == CKR_OK)
					*pLength = ((*pLength + 7)/8) * 2 ; /* 2*nLen in bytes */
				break;
			case CKK_GOSTR3410:
				rv = key->ops->get_attribute(operation->session, key, &attr);
				if (rv == CKR_OK)
					*pLength = (*pLength + 7) / 8 * 2;
				break;
			default:
				rv = CKR_MECHANISM_INVALID;
		}
	}

	LOG_FUNC_RETURN(context, (int) rv);
}

static void
sc_pkcs11_operation_release(sc_pkcs11_operation_t *operation)
{
	if (!operation)
	    return;
	operation_data_release(operation->priv_data);
}

#ifdef ENABLE_OPENSSL
/*
 * Initialize a verify context. When we get here, we know
 * the key object is capable of verifying _something_
 */
CK_RV
sc_pkcs11_verif_init(struct sc_pkcs11_session *session, CK_MECHANISM_PTR pMechanism,
		struct sc_pkcs11_object *key, CK_KEY_TYPE key_type)
{
	struct sc_pkcs11_card *p11card;
	sc_pkcs11_operation_t *operation;
	sc_pkcs11_mechanism_type_t *mt;
	CK_RV rv;

	if (!session || !session->slot
	 || !(p11card = session->slot->p11card))
		return CKR_ARGUMENTS_BAD;

	/* See if we support this mechanism type */
	mt = sc_pkcs11_find_mechanism(p11card, pMechanism->mechanism, CKF_VERIFY);
	if (mt == NULL)
		return CKR_MECHANISM_INVALID;

	/* See if compatible with key type */
	rv = _validate_key_type(mt, key_type);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	rv = session_start_operation(session, SC_PKCS11_OPERATION_VERIFY, mt, &operation);
	if (rv != CKR_OK)
		return rv;

	memcpy(&operation->mechanism, pMechanism, sizeof(CK_MECHANISM));
	if (pMechanism->pParameter) {
		memcpy(&operation->mechanism_params, pMechanism->pParameter,
			pMechanism->ulParameterLen);
		operation->mechanism.pParameter = &operation->mechanism_params;
	}

	rv = mt->verif_init(operation, key);

	if (rv != CKR_OK)
		session_stop_operation(session, SC_PKCS11_OPERATION_VERIFY);

	return rv;

}

CK_RV
sc_pkcs11_verif_update(struct sc_pkcs11_session *session,
		      CK_BYTE_PTR pData, CK_ULONG ulDataLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_VERIFY, &op);
	if (rv != CKR_OK)
		return rv;

	if (op->type->verif_update == NULL) {
		rv = CKR_KEY_TYPE_INCONSISTENT;
		goto done;
	}

	rv = op->type->verif_update(op, pData, ulDataLen);

done:
	if (rv != CKR_OK)
		session_stop_operation(session, SC_PKCS11_OPERATION_VERIFY);

	return rv;
}

CK_RV
sc_pkcs11_verif_final(struct sc_pkcs11_session *session,
		     CK_BYTE_PTR pSignature, CK_ULONG ulSignatureLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_VERIFY, &op);
	if (rv != CKR_OK)
		return rv;

	if (op->type->verif_final == NULL) {
		rv = CKR_KEY_TYPE_INCONSISTENT;
		goto done;
	}

	rv = op->type->verif_final(op, pSignature, ulSignatureLen);

done:
	session_stop_operation(session, SC_PKCS11_OPERATION_VERIFY);
	return rv;
}

/*
 * Initialize a verify operation
 */
static CK_RV
sc_pkcs11_verify_init(sc_pkcs11_operation_t *operation,
		    struct sc_pkcs11_object *key)
{
	struct hash_signature_info *info;
	struct operation_data *data;
	CK_RV rv;

	if (!(data = new_operation_data()))
		return CKR_HOST_MEMORY;

	data->info = NULL;
	data->key = key;

	if (key->ops->can_do)   {
		rv = key->ops->can_do(operation->session, key, operation->type->mech, CKF_SIGN);
		if ((rv == CKR_OK) || (rv == CKR_FUNCTION_NOT_SUPPORTED))   {
			/* Mechanism recognized and can be performed by pkcs#15 card or algorithm references not supported */
		}
		else {
			/* Mechanism cannot be performed by pkcs#15 card, or some general error. */
			free(data);
			LOG_FUNC_RETURN(context, (int) rv);
		}
	}

	/* Validate the mechanism parameters */
	if (key->ops->init_params) {
		rv = key->ops->init_params(operation->session, &operation->mechanism);
		if (rv != CKR_OK) {
			/* Probably bad arguments */
			free(data);
			LOG_FUNC_RETURN(context, (int) rv);
		}
	}

	/* If this is a verify with hash operation, set up the
	 * hash operation */
	info = (struct hash_signature_info *) operation->type->mech_data;
	if (info != NULL) {
		/* Initialize hash operation */
		data->md = sc_pkcs11_new_operation(operation->session,
						   info->hash_type);
		if (data->md == NULL)
			rv = CKR_HOST_MEMORY;
		else
			rv = info->hash_type->md_init(data->md);
		if (rv != CKR_OK) {
			sc_pkcs11_release_operation(&data->md);
			free(data);
			return rv;
		}
		data->info = info;
	}

	operation->priv_data = data;
	return CKR_OK;
}

static CK_RV
sc_pkcs11_verify_update(sc_pkcs11_operation_t *operation,
		    CK_BYTE_PTR pPart, CK_ULONG ulPartLen)
{
	struct operation_data *data;

	data = (struct operation_data *)operation->priv_data;
	if (data->md) {
		sc_pkcs11_operation_t	*md = data->md;

		return md->type->md_update(md, pPart, ulPartLen);
	}

	/* This verification mechanism operates on the raw data */
	CK_RV rv = signature_data_buffer_append(data, pPart, ulPartLen);
	LOG_FUNC_RETURN(context, (int) rv);
}

static CK_RV
sc_pkcs11_verify_final(sc_pkcs11_operation_t *operation,
			CK_BYTE_PTR pSignature, CK_ULONG ulSignatureLen)
{
	struct operation_data *data;
	struct sc_pkcs11_object *key;
	unsigned char *pubkey_value = NULL;
	CK_KEY_TYPE key_type;
	CK_BYTE params[9 /* GOST_PARAMS_ENCODED_OID_SIZE */] = { 0 };
	CK_ATTRIBUTE attr = {CKA_VALUE, NULL, 0};
	CK_ATTRIBUTE attr_key_type = {CKA_KEY_TYPE, &key_type, sizeof(key_type)};
	CK_ATTRIBUTE attr_key_params = {CKA_GOSTR3410_PARAMS, &params, sizeof(params)};
	CK_RV rv;

	data = (struct operation_data *)operation->priv_data;

	if (pSignature == NULL)
		return CKR_ARGUMENTS_BAD;

	key = data->key;
	rv = key->ops->get_attribute(operation->session, key, &attr_key_type);
	if (rv != CKR_OK)
		return rv;

	if (key_type != CKK_GOSTR3410)
		attr.type = CKA_SPKI;
		

	rv = key->ops->get_attribute(operation->session, key, &attr);
	if (rv != CKR_OK)
		return rv;
	pubkey_value = calloc(1, attr.ulValueLen);
	if (!pubkey_value) {
		rv = CKR_HOST_MEMORY;
		goto done;
	}
	attr.pValue = pubkey_value;
	rv = key->ops->get_attribute(operation->session, key, &attr);
	if (rv != CKR_OK)
		goto done;

	if (key_type == CKK_GOSTR3410) {
		rv = key->ops->get_attribute(operation->session, key, &attr_key_params);
		if (rv != CKR_OK)
			goto done;
	}

	rv = sc_pkcs11_verify_data(pubkey_value, (unsigned int) attr.ulValueLen,
		params, sizeof(params),
		&operation->mechanism, data->md,
		data->buffer, data->buffer_len, pSignature, (unsigned int) ulSignatureLen);

done:
	free(pubkey_value);

	return rv;
}
#endif
/*
 * Initialize a encrypting context. When we get here, we know
 * the key object is capable of encrypt _something_
 */
CK_RV
sc_pkcs11_encr_init(struct sc_pkcs11_session *session,
		CK_MECHANISM_PTR pMechanism,
		struct sc_pkcs11_object *key,
		CK_KEY_TYPE key_type)
{
	struct sc_pkcs11_card *p11card;
	sc_pkcs11_operation_t *operation;
	sc_pkcs11_mechanism_type_t *mt;
	CK_RV rv;

	if (!session || !session->slot || !(p11card = session->slot->p11card))
		return CKR_ARGUMENTS_BAD;

	/* See if we support this mechanism type */
	mt = sc_pkcs11_find_mechanism(p11card, pMechanism->mechanism, CKF_ENCRYPT);
	if (mt == NULL)
		return CKR_MECHANISM_INVALID;

	/* See if compatible with key type */
	rv = _validate_key_type(mt, key_type);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int)rv);

	rv = session_start_operation(session, SC_PKCS11_OPERATION_ENCRYPT, mt, &operation);
	if (rv != CKR_OK)
		return rv;

	memcpy(&operation->mechanism, pMechanism, sizeof(CK_MECHANISM));
	if (pMechanism->pParameter) {
		memcpy(&operation->mechanism_params, pMechanism->pParameter,
				pMechanism->ulParameterLen);
		operation->mechanism.pParameter = &operation->mechanism_params;
	}
	rv = mt->encrypt_init(operation, key);
	if (rv != CKR_OK)
		goto out;

	/* Validate the mechanism parameters */
	if (key->ops->init_params) {
		rv = key->ops->init_params(operation->session, &operation->mechanism);
		if (rv != CKR_OK)
			goto out;
	}
	LOG_FUNC_RETURN(context, (int)rv);
out:
	session_stop_operation(session, SC_PKCS11_OPERATION_ENCRYPT);
	LOG_FUNC_RETURN(context, (int)rv);
}

CK_RV
sc_pkcs11_encr(struct sc_pkcs11_session *session,
		CK_BYTE_PTR pData, CK_ULONG ulDataLen,
		CK_BYTE_PTR pEncryptedData, CK_ULONG_PTR pulEncryptedDataLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_ENCRYPT, &op);
	if (rv != CKR_OK)
		return rv;

	rv = op->type->encrypt(op, pData, ulDataLen,
			pEncryptedData, pulEncryptedDataLen);

	/* application is requesting buffer size ? */
	if (pEncryptedData == NULL) {
		/* do not terminate session for CKR_OK */
		if (rv == CKR_OK)
			LOG_FUNC_RETURN(context, CKR_OK);
	} else if (rv == CKR_BUFFER_TOO_SMALL)
		LOG_FUNC_RETURN(context, CKR_BUFFER_TOO_SMALL);

	session_stop_operation(session, SC_PKCS11_OPERATION_ENCRYPT);
	LOG_FUNC_RETURN(context, (int)rv);
}

CK_RV
sc_pkcs11_encr_update(struct sc_pkcs11_session *session,
		CK_BYTE_PTR pData, CK_ULONG ulDataLen,
		CK_BYTE_PTR pEncryptedData, CK_ULONG_PTR pulEncryptedDataLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_ENCRYPT, &op);
	if (rv != CKR_OK)
		return rv;

	rv = op->type->encrypt_update(op, pData, ulDataLen,
			pEncryptedData, pulEncryptedDataLen);

	/* terminate session for any error except CKR_BUFFER_TOO_SMALL */
	if (rv != CKR_OK && rv != CKR_BUFFER_TOO_SMALL)
		session_stop_operation(session, SC_PKCS11_OPERATION_ENCRYPT);
	LOG_FUNC_RETURN(context, (int)rv);
}

CK_RV
sc_pkcs11_encr_final(struct sc_pkcs11_session *session,
		CK_BYTE_PTR pEncryptedData, CK_ULONG_PTR pulEncryptedDataLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_ENCRYPT, &op);
	if (rv != CKR_OK)
		return rv;

	rv = op->type->encrypt_final(op, pEncryptedData, pulEncryptedDataLen);

	/* application is requesting buffer size ? */
	if (pEncryptedData == NULL) {
		/* do not terminate session for CKR_OK */
		if (rv == CKR_OK)
			LOG_FUNC_RETURN(context, CKR_OK);
	} else if (rv == CKR_BUFFER_TOO_SMALL)
		LOG_FUNC_RETURN(context, CKR_BUFFER_TOO_SMALL);

	session_stop_operation(session, SC_PKCS11_OPERATION_ENCRYPT);
	LOG_FUNC_RETURN(context, (int)rv);
}

/*
 * Initialize a decryption context. When we get here, we know
 * the key object is capable of decrypting _something_
 */
CK_RV
sc_pkcs11_decr_init(struct sc_pkcs11_session *session,
			CK_MECHANISM_PTR pMechanism,
			struct sc_pkcs11_object *key,
			CK_KEY_TYPE key_type)
{
	struct sc_pkcs11_card *p11card;
	sc_pkcs11_operation_t *operation;
	sc_pkcs11_mechanism_type_t *mt;
	CK_RV rv;

	if (!session || !session->slot
	 || !(p11card = session->slot->p11card))
		return CKR_ARGUMENTS_BAD;

	/* See if we support this mechanism type */
	mt = sc_pkcs11_find_mechanism(p11card, pMechanism->mechanism, CKF_DECRYPT);
	if (mt == NULL)
		return CKR_MECHANISM_INVALID;

	/* See if compatible with key type */
	rv = _validate_key_type(mt, key_type);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	rv = session_start_operation(session, SC_PKCS11_OPERATION_DECRYPT, mt, &operation);
	if (rv != CKR_OK)
		return rv;

	memcpy(&operation->mechanism, pMechanism, sizeof(CK_MECHANISM));
	if (pMechanism->pParameter) {
		memcpy(&operation->mechanism_params, pMechanism->pParameter,
		       pMechanism->ulParameterLen);
		operation->mechanism.pParameter = &operation->mechanism_params;
	}
	rv = mt->decrypt_init(operation, key);

	/* Validate the mechanism parameters */
	if (key->ops->init_params) {
		rv = key->ops->init_params(operation->session, &operation->mechanism);
		if (rv != CKR_OK) {
			/* Probably bad arguments */
			LOG_FUNC_RETURN(context, (int) rv);
		}
	}

	if (rv != CKR_OK)
		session_stop_operation(session, SC_PKCS11_OPERATION_DECRYPT);

	return rv;
}

CK_RV
sc_pkcs11_decr(struct sc_pkcs11_session *session,
		CK_BYTE_PTR pEncryptedData, CK_ULONG ulEncryptedDataLen,
		CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_DECRYPT, &op);
	if (rv != CKR_OK)
		return rv;

	rv = op->type->decrypt(op, pEncryptedData, ulEncryptedDataLen,
	                       pData, pulDataLen);

	if (rv != CKR_BUFFER_TOO_SMALL && pData != NULL)
		session_stop_operation(session, SC_PKCS11_OPERATION_DECRYPT);

	return rv;
}

CK_RV
sc_pkcs11_decr_update(struct sc_pkcs11_session *session,
		CK_BYTE_PTR pEncryptedData, CK_ULONG ulEncryptedDataLen,
		CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_DECRYPT, &op);
	if (rv != CKR_OK)
		return rv;

	rv = op->type->decrypt_update(op, pEncryptedData, ulEncryptedDataLen,
			pData, pulDataLen);

	/* terminate session for any error except CKR_BUFFER_TOO_SMALL */
	if (rv != CKR_OK && rv != CKR_BUFFER_TOO_SMALL)
		session_stop_operation(session, SC_PKCS11_OPERATION_DECRYPT);
	LOG_FUNC_RETURN(context, (int)rv);
}

CK_RV
sc_pkcs11_decr_final(struct sc_pkcs11_session *session,
		CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen)
{
	sc_pkcs11_operation_t *op;
	CK_RV rv;

	rv = session_get_operation(session, SC_PKCS11_OPERATION_DECRYPT, &op);
	if (rv != CKR_OK)
		return rv;

	rv = op->type->decrypt_final(op, pData, pulDataLen);

	/* application is requesting buffer size ? */
	if (pData == NULL) {
		/* do not terminate session for CKR_OK */
		if (rv == CKR_OK)
			LOG_FUNC_RETURN(context, CKR_OK);
	} else if (rv == CKR_BUFFER_TOO_SMALL)
		LOG_FUNC_RETURN(context, CKR_BUFFER_TOO_SMALL);

	session_stop_operation(session, SC_PKCS11_OPERATION_DECRYPT);
	LOG_FUNC_RETURN(context, (int)rv);
}

CK_RV
sc_pkcs11_wrap(struct sc_pkcs11_session *session,
	CK_MECHANISM_PTR pMechanism,
	struct sc_pkcs11_object *wrappingKey,	/* wrapping key */
	CK_KEY_TYPE key_type,			/* type of the wrapping key */
	struct sc_pkcs11_object *targetKey,	/* the key to be wrapped */
	CK_BYTE_PTR wrappedData,
	CK_ULONG_PTR wrappedDataLen)
{
	struct sc_pkcs11_card *p11card;
	sc_pkcs11_operation_t *operation;
	sc_pkcs11_mechanism_type_t *mt;
	CK_RV rv;

	if (!session || !session->slot || !(p11card = session->slot->p11card))
		return CKR_ARGUMENTS_BAD;

	/* See if we support this mechanism type */
	mt = sc_pkcs11_find_mechanism(p11card, pMechanism->mechanism, CKF_UNWRAP);
	if (mt == NULL)
		return CKR_MECHANISM_INVALID;

	/* See if compatible with key type */
	rv = _validate_key_type(mt, key_type);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	rv = session_start_operation(session, SC_PKCS11_OPERATION_WRAP, mt, &operation);
	if (rv != CKR_OK)
		return rv;

	memcpy(&operation->mechanism, pMechanism, sizeof(CK_MECHANISM));

	rv = operation->type->wrap(operation, wrappingKey,
			targetKey, wrappedData,
			wrappedDataLen);

	session_stop_operation(session, SC_PKCS11_OPERATION_WRAP);

	return rv;
}

/*
 * Unwrap a wrapped key into card. A new key object is created on card.
 */
CK_RV
sc_pkcs11_unwrap(struct sc_pkcs11_session *session,
	CK_MECHANISM_PTR pMechanism,
	struct sc_pkcs11_object *unwrappingKey,
	CK_KEY_TYPE key_type, /* type of the unwrapping key */
	CK_BYTE_PTR pWrappedKey,	/* the wrapped key */
	CK_ULONG ulWrappedKeyLen,	/* bytes length of wrapped key */
	struct sc_pkcs11_object *targetKey)
{
	struct sc_pkcs11_card *p11card;
	sc_pkcs11_operation_t *operation;
	sc_pkcs11_mechanism_type_t *mt;

	CK_RV rv;

	if (!session || !session->slot || !(p11card = session->slot->p11card))
		return CKR_ARGUMENTS_BAD;

	/* See if we support this mechanism type */
	mt = sc_pkcs11_find_mechanism(p11card, pMechanism->mechanism, CKF_UNWRAP);
	if (mt == NULL)
		return CKR_MECHANISM_INVALID;

	/* See if compatible with key type */
	rv = _validate_key_type(mt, key_type);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	rv = session_start_operation(session, SC_PKCS11_OPERATION_UNWRAP, mt, &operation);
	if (rv != CKR_OK)
		return rv;

	memcpy(&operation->mechanism, pMechanism, sizeof(CK_MECHANISM));


	/*
	 *  TODO: does it make sense to support unwrapping to an in memory key object?
	 *  This implementation assumes that the key should be unwrapped into a
	 *  key object on card, regardless whether CKA_TOKEN = FALSE
	 *  CKA_TOKEN = FALSE is considered an on card session object.
	 */

	rv = operation->type->unwrap(operation, unwrappingKey,
			pWrappedKey, ulWrappedKeyLen,
			targetKey);

	session_stop_operation(session, SC_PKCS11_OPERATION_UNWRAP);

	return rv;
}

/* Derive one key from another, and return results in created object */
CK_RV
sc_pkcs11_deri(struct sc_pkcs11_session *session,
	CK_MECHANISM_PTR pMechanism,
	struct sc_pkcs11_object * basekey,
	CK_KEY_TYPE key_type,
	CK_SESSION_HANDLE hSession,
	CK_OBJECT_HANDLE hdkey,
	struct sc_pkcs11_object * dkey)
{

	struct sc_pkcs11_card *p11card;
	sc_pkcs11_operation_t *operation;
	sc_pkcs11_mechanism_type_t *mt;
	CK_BYTE_PTR keybuf = NULL;
	CK_ULONG ulDataLen = 0;
	CK_ATTRIBUTE template[] = {
		{CKA_VALUE, keybuf, 0}
	};

	CK_RV rv;


	if (!session || !session->slot
	 || !(p11card = session->slot->p11card))
		return CKR_ARGUMENTS_BAD;

	/* See if we support this mechanism type */
	mt = sc_pkcs11_find_mechanism(p11card, pMechanism->mechanism, CKF_DERIVE);
	if (mt == NULL)
		return CKR_MECHANISM_INVALID;

	/* See if compatible with key type */
	rv = _validate_key_type(mt, key_type);
	if (rv != CKR_OK)
		LOG_FUNC_RETURN(context, (int) rv);

	rv = session_start_operation(session, SC_PKCS11_OPERATION_DERIVE, mt, &operation);
	if (rv != CKR_OK)
		return rv;

	memcpy(&operation->mechanism, pMechanism, sizeof(CK_MECHANISM));

	/* Get the size of the data to be returned
	 * If the card could derive a key an leave it on the card
	 * then no data is returned.
	 * If the card returns the data, we will store it in the secret key CKA_VALUE
	 */

	ulDataLen = 0;
	rv = operation->type->derive(operation, basekey,
			pMechanism->pParameter, pMechanism->ulParameterLen,
			NULL, &ulDataLen);
	if (rv != CKR_OK)
		goto out;

	if (ulDataLen > 0)
		keybuf = calloc(1,ulDataLen);
	else
		keybuf = calloc(1,8); /* pass in  dummy buffer */

	if (!keybuf) {
		rv = CKR_HOST_MEMORY;
		goto out;
	}

	/* Now do the actual derivation */

	rv = operation->type->derive(operation, basekey,
	    pMechanism->pParameter, pMechanism->ulParameterLen,
	    keybuf, &ulDataLen);
	if (rv != CKR_OK)
	    goto out;


/* add the CKA_VALUE attribute to the template if it was returned
 * if not assume it is on the card...
 * But for now PIV with ECDH returns the generic key data
 * TODO need to support truncation, if CKA_VALUE_LEN < ulDataLem
 */
	if (ulDataLen > 0) {
	    template[0].pValue = keybuf;
	    template[0].ulValueLen = ulDataLen;

	    dkey->ops->set_attribute(session, dkey, &template[0]);

	    memset(keybuf,0,ulDataLen);
	}

out:
	session_stop_operation(session, SC_PKCS11_OPERATION_DERIVE);

	if (keybuf)
	    free(keybuf);
	return rv;
}

/*
 * Initialize a encrypt operation
 */
static CK_RV
sc_pkcs11_encrypt_init(sc_pkcs11_operation_t *operation,
		struct sc_pkcs11_object *key)
{
	struct operation_data *data;
	CK_RV rv;

	if (!(data = new_operation_data()))
		return CKR_HOST_MEMORY;

	data->key = key;

	if (key->ops->can_do) {
		rv = key->ops->can_do(operation->session, key, operation->type->mech, CKF_ENCRYPT);
		if ((rv == CKR_OK) || (rv == CKR_FUNCTION_NOT_SUPPORTED)) {
			/* Mechanism recognized and can be performed by pkcs#15 card or algorithm references not supported */
		} else {
			/* Mechanism cannot be performed by pkcs#15 card, or some general error. */
			free(data);
			LOG_FUNC_RETURN(context, (int)rv);
		}
	}

	operation->priv_data = data;

	/* The last parameter is NULL - this is call to INIT code in underlying functions */
	return key->ops->encrypt(operation->session,
			key, &operation->mechanism, NULL, 0, NULL, NULL);
}

static CK_RV
sc_pkcs11_encrypt(sc_pkcs11_operation_t *operation,
		CK_BYTE_PTR pData, CK_ULONG ulDataLen,
		CK_BYTE_PTR pEncryptedData, CK_ULONG_PTR pulEncryptedDataLen)
{
	struct operation_data *data;
	struct sc_pkcs11_object *key;
	CK_RV rv;
	CK_ULONG ulEncryptedDataLen, ulLastEncryptedPartLen;

	/* PKCS#11: If pBuf is not NULL_PTR, then *pulBufLen must contain the size in bytes.. */
	if (pEncryptedData && !pulEncryptedDataLen)
		return CKR_ARGUMENTS_BAD;

	ulEncryptedDataLen = pulEncryptedDataLen ? *pulEncryptedDataLen : 0;
	ulLastEncryptedPartLen = ulEncryptedDataLen;

	data = (struct operation_data *)operation->priv_data;

	key = data->key;

	/* Encrypt (Update) */
	rv = key->ops->encrypt(operation->session, key, &operation->mechanism,
			pData, ulDataLen, pEncryptedData, &ulEncryptedDataLen);

	if (pulEncryptedDataLen)
		*pulEncryptedDataLen = ulEncryptedDataLen;

	if (rv != CKR_OK)
		return rv;

	/* recalculate buffer space */
	if (ulEncryptedDataLen <= ulLastEncryptedPartLen)
		ulLastEncryptedPartLen -= ulEncryptedDataLen;
	else
		ulLastEncryptedPartLen = 0;

	/* EncryptFinalize */
	rv = key->ops->encrypt(operation->session, key, &operation->mechanism,
			NULL, 0, pEncryptedData + ulEncryptedDataLen, &ulLastEncryptedPartLen);

	if (pulEncryptedDataLen)
		*pulEncryptedDataLen = ulEncryptedDataLen + ulLastEncryptedPartLen;
	return rv;
}

static CK_RV
sc_pkcs11_encrypt_update(sc_pkcs11_operation_t *operation,
		CK_BYTE_PTR pPart, CK_ULONG ulPartLen,
		CK_BYTE_PTR pEncryptedPart, CK_ULONG_PTR pulEncryptedPartLen)
{
	struct operation_data *data;
	struct sc_pkcs11_object *key;
	CK_RV rv;
	CK_ULONG ulEncryptedPartLen;

	/* PKCS#11: If pBuf is not NULL_PTR, then *pulBufLen must contain the size in bytes.. */
	if (pEncryptedPart && !pulEncryptedPartLen)
		return CKR_ARGUMENTS_BAD;

	ulEncryptedPartLen = pulEncryptedPartLen ? *pulEncryptedPartLen : 0;

	data = (struct operation_data *)operation->priv_data;

	key = data->key;

	rv = key->ops->encrypt(operation->session, key, &operation->mechanism,
			pPart, ulPartLen, pEncryptedPart, &ulEncryptedPartLen);

	if (pulEncryptedPartLen)
		*pulEncryptedPartLen = ulEncryptedPartLen;
	return rv;
}

static CK_RV
sc_pkcs11_encrypt_final(sc_pkcs11_operation_t *operation,
		CK_BYTE_PTR pLastEncryptedPart,
		CK_ULONG_PTR pulLastEncryptedPartLen)
{
	struct operation_data *data;
	struct sc_pkcs11_object *key;
	CK_RV rv;
	CK_ULONG ulLastEncryptedPartLen;

	/* PKCS#11: If pBuf is not NULL_PTR, then *pulBufLen must contain the size in bytes.. */
	if (pLastEncryptedPart && !pulLastEncryptedPartLen)
		return CKR_ARGUMENTS_BAD;

	ulLastEncryptedPartLen = pulLastEncryptedPartLen ? *pulLastEncryptedPartLen : 0;

	data = (struct operation_data *)operation->priv_data;

	key = data->key;

	rv = key->ops->encrypt(operation->session, key, &operation->mechanism,
			NULL, 0, pLastEncryptedPart, &ulLastEncryptedPartLen);

	if (pulLastEncryptedPartLen)
		*pulLastEncryptedPartLen = ulLastEncryptedPartLen;
	return rv;
}

/*
 * Initialize a decrypt operation
 */
static CK_RV
sc_pkcs11_decrypt_init(sc_pkcs11_operation_t *operation,
			struct sc_pkcs11_object *key)
{
	struct operation_data *data;
	CK_RV rv;

	if (!(data = new_operation_data()))
		return CKR_HOST_MEMORY;

	data->key = key;

	if (key->ops->can_do)   {
		rv = key->ops->can_do(operation->session, key, operation->type->mech, CKF_DECRYPT);
		if ((rv == CKR_OK) || (rv == CKR_FUNCTION_NOT_SUPPORTED))   {
			/* Mechanism recognized and can be performed by pkcs#15 card or algorithm references not supported */
		}
		else {
			/* Mechanism cannot be performed by pkcs#15 card, or some general error. */
			free(data);
			LOG_FUNC_RETURN(context, (int) rv);
		}
	}
	operation->priv_data = data;

	/* The last parameter is NULL - this is call to INIT code in underlying functions */
	return key->ops->decrypt(operation->session,
			key, &operation->mechanism, NULL, 0, NULL, NULL);
}

static CK_RV
sc_pkcs11_decrypt(sc_pkcs11_operation_t *operation,
		CK_BYTE_PTR pEncryptedData, CK_ULONG ulEncryptedDataLen,
		CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen)
{
	struct operation_data *data;
	struct sc_pkcs11_object *key;
	CK_RV rv;
	CK_ULONG ulDataLen, ulLastDataLen;

	/* PKCS#11: If pBuf is not NULL_PTR, then *pulBufLen must contain the size in bytes.. */
	if (pData && !pulDataLen)
		return CKR_ARGUMENTS_BAD;

	ulDataLen = pulDataLen ? *pulDataLen : 0;
	ulLastDataLen = ulDataLen;

	data = (struct operation_data *)operation->priv_data;

	key = data->key;

	/* Decrypt */
	rv = key->ops->decrypt(operation->session, key, &operation->mechanism,
			pEncryptedData, ulEncryptedDataLen, pData, &ulDataLen);

	if (pulDataLen)
		*pulDataLen = ulDataLen;

	if (rv != CKR_OK)
		return rv;

	/* recalculate buffer space */
	if (ulDataLen <= ulLastDataLen)
		ulLastDataLen -= ulDataLen;
	else
		ulLastDataLen = 0;

	/* DecryptFinalize */
	rv = key->ops->decrypt(operation->session, key, &operation->mechanism,
			NULL, 0, pData + ulDataLen, &ulLastDataLen);
	if (pulDataLen)
		*pulDataLen = ulDataLen + ulLastDataLen;
	return rv;
}

static CK_RV
sc_pkcs11_decrypt_update(sc_pkcs11_operation_t *operation,
		CK_BYTE_PTR pEncryptedPart, CK_ULONG ulEncryptedPartLen,
		CK_BYTE_PTR pPart, CK_ULONG_PTR pulPartLen)
{
	struct operation_data *data;
	struct sc_pkcs11_object *key;
	CK_RV rv;
	CK_ULONG ulPartLen;

	/* PKCS#11: If pBuf is not NULL_PTR, then *pulBufLen must contain the size in bytes.. */
	if (pPart && !pulPartLen)
		return CKR_ARGUMENTS_BAD;

	ulPartLen = pulPartLen ? *pulPartLen : 0;

	data = (struct operation_data *)operation->priv_data;

	key = data->key;

	rv = key->ops->decrypt(operation->session,
			key, &operation->mechanism,
			pEncryptedPart, ulEncryptedPartLen,
			pPart, &ulPartLen);

	if (pulPartLen)
		*pulPartLen = ulPartLen;
	return rv;
}

static CK_RV
sc_pkcs11_decrypt_final(sc_pkcs11_operation_t *operation,
		CK_BYTE_PTR pLastPart,
		CK_ULONG_PTR pulLastPartLen)
{
	struct operation_data *data;
	struct sc_pkcs11_object *key;
	CK_RV rv;
	CK_ULONG ulLastPartLen;

	/* PKCS#11: If pBuf is not NULL_PTR, then *pulBufLen must contain the size in bytes.. */
	if (pLastPart && !pulLastPartLen)
		return CKR_ARGUMENTS_BAD;

	ulLastPartLen = pulLastPartLen ? *pulLastPartLen : 0;

	data = (struct operation_data *)operation->priv_data;

	key = data->key;

	rv = key->ops->decrypt(operation->session,
			key, &operation->mechanism,
			NULL, 0,
			pLastPart, &ulLastPartLen);

	if (pulLastPartLen)
		*pulLastPartLen = ulLastPartLen;
	return rv;
}

static CK_RV
sc_pkcs11_derive(sc_pkcs11_operation_t *operation,
	    struct sc_pkcs11_object *basekey,
	    CK_BYTE_PTR pmechParam, CK_ULONG ulmechParamLen,
	    CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen)
{

	return basekey->ops->derive(operation->session,
		    basekey,
		    &operation->mechanism,
		    pmechParam, ulmechParamLen,
		    pData, pulDataLen);
}


static CK_RV
sc_pkcs11_wrap_operation(sc_pkcs11_operation_t *operation,
	    struct sc_pkcs11_object *wrappingKey,
	    struct sc_pkcs11_object *targetKey,
	    CK_BYTE_PTR pWrappedData, CK_ULONG_PTR ulWrappedDataLen)
{
	if (!operation || !wrappingKey || !wrappingKey->ops || !wrappingKey->ops->wrap_key)
		return CKR_ARGUMENTS_BAD;

    return wrappingKey->ops->wrap_key(operation->session,
		    wrappingKey,
		    &operation->mechanism,
		    targetKey, pWrappedData,
		    ulWrappedDataLen);
}

static CK_RV
sc_pkcs11_unwrap_operation(sc_pkcs11_operation_t *operation,
	    struct sc_pkcs11_object *unwrappingKey,
	    CK_BYTE_PTR pWrappedKey, CK_ULONG ulWrappedKeyLen,
	    struct sc_pkcs11_object *targetKey)
{
	if (!operation || !unwrappingKey || !unwrappingKey->ops || !unwrappingKey->ops->unwrap_key)
		return CKR_ARGUMENTS_BAD;

    return unwrappingKey->ops->unwrap_key(operation->session,
		    unwrappingKey,
		    &operation->mechanism,
		    pWrappedKey, ulWrappedKeyLen,
		    targetKey);
}

/*
 * Create new mechanism type for a mechanism supported by
 * the card
 */
sc_pkcs11_mechanism_type_t *
sc_pkcs11_new_fw_mechanism(CK_MECHANISM_TYPE mech,
				CK_MECHANISM_INFO_PTR pInfo,
				CK_KEY_TYPE key_type,
				const void *priv_data,
				void (*free_priv_data)(const void *priv_data),
				CK_RV (*copy_priv_data)(const void *mech_data, void **new_data))
{
	sc_pkcs11_mechanism_type_t *mt;

	mt = calloc(1, sizeof(*mt));
	if (mt == NULL)
		return mt;
	mt->mech = mech;
	mt->mech_info = *pInfo;
	mt->key_types[0] = (int)key_type;
	mt->key_types[1] = -1;
	mt->mech_data = priv_data;
	mt->free_mech_data = free_priv_data;
	mt->copy_mech_data = copy_priv_data;
	mt->obj_size = sizeof(sc_pkcs11_operation_t);

	mt->release = sc_pkcs11_operation_release;

	if (pInfo->flags & CKF_SIGN) {
		mt->sign_init = sc_pkcs11_signature_init;
		mt->sign_update = sc_pkcs11_signature_update;
		mt->sign_final = sc_pkcs11_signature_final;
		mt->sign_size = sc_pkcs11_signature_size;
#ifdef ENABLE_OPENSSL
		mt->verif_init = sc_pkcs11_verify_init;
		mt->verif_update = sc_pkcs11_verify_update;
		mt->verif_final = sc_pkcs11_verify_final;
#endif
	}
	if (pInfo->flags & CKF_WRAP) {
		mt->wrap = sc_pkcs11_wrap_operation;
	}
	if (pInfo->flags & CKF_UNWRAP) {
		mt->unwrap = sc_pkcs11_unwrap_operation;
	}
	if (pInfo->flags & CKF_DERIVE) {
		mt->derive = sc_pkcs11_derive;
	}
	if (pInfo->flags & CKF_DECRYPT) {
		mt->decrypt_init = sc_pkcs11_decrypt_init;
		mt->decrypt = sc_pkcs11_decrypt;
		mt->decrypt_update = sc_pkcs11_decrypt_update;
		mt->decrypt_final = sc_pkcs11_decrypt_final;
	}
	if (pInfo->flags & CKF_ENCRYPT) {
		mt->encrypt_init = sc_pkcs11_encrypt_init;
		mt->encrypt = sc_pkcs11_encrypt;
		mt->encrypt_update = sc_pkcs11_encrypt_update;
		mt->encrypt_final = sc_pkcs11_encrypt_final;
	}

	return mt;
}

void sc_pkcs11_free_mechanism(sc_pkcs11_mechanism_type_t **mt)
{
	if (!mt || !(*mt))
		return;
	if ((*mt)->free_mech_data)
		(*mt)->free_mech_data((*mt)->mech_data);
	free(*mt);
	*mt = NULL;
}

/*
 * Register generic mechanisms
 */
CK_RV
sc_pkcs11_register_generic_mechanisms(struct sc_pkcs11_card *p11card)
{
#ifdef ENABLE_OPENSSL
	sc_pkcs11_register_openssl_mechanisms(p11card);
#endif
	return CKR_OK;
}

void free_info(const void *info)
{
	free((void *) info);
}

CK_RV copy_hash_signature_info(const void *mech_data, void **new_data)
{
	if (mech_data == NULL || new_data == NULL)
		return CKR_ARGUMENTS_BAD;

	*new_data = calloc(1, sizeof(struct hash_signature_info));
	if (!(*new_data))
		return CKR_HOST_MEMORY;
	
	memcpy(*new_data, mech_data, sizeof(struct hash_signature_info));
	return CKR_OK;
}

/*
 * Register a sign+hash algorithm derived from an algorithm supported
 * by the token + a software hash mechanism
 */
CK_RV
sc_pkcs11_register_sign_and_hash_mechanism(struct sc_pkcs11_card *p11card,
		CK_MECHANISM_TYPE mech,
		CK_MECHANISM_TYPE hash_mech,
		sc_pkcs11_mechanism_type_t *sign_type)
{
	sc_pkcs11_mechanism_type_t *hash_type, *new_type;
	struct hash_signature_info *info;
	CK_MECHANISM_INFO mech_info;
	CK_RV rv;

	if (!sign_type)
		return CKR_MECHANISM_INVALID;
	mech_info = sign_type->mech_info;

	if (!(hash_type = sc_pkcs11_find_mechanism(p11card, hash_mech, CKF_DIGEST)))
		return CKR_MECHANISM_INVALID;

	/* These hash-based mechs can only be used for sign/verify */
	mech_info.flags &= (CKF_SIGN | CKF_SIGN_RECOVER | CKF_VERIFY | CKF_VERIFY_RECOVER);

	info = calloc(1, sizeof(*info));
	if (!info)
		return CKR_HOST_MEMORY;

	info->mech = mech;
	info->hash_type = hash_type;
	info->sign_mech = sign_type->mech;
	info->hash_mech = hash_mech;

	new_type = sc_pkcs11_new_fw_mechanism(mech, &mech_info, sign_type->key_types[0], info, free_info, copy_hash_signature_info);
	if (!new_type) {
		free_info(info);
		return CKR_HOST_MEMORY;
	}

	rv = sc_pkcs11_register_mechanism(p11card, new_type, NULL);
	sc_pkcs11_free_mechanism(&new_type);

	return rv;
}
