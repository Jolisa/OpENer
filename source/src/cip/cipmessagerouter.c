/*******************************************************************************
 * Copyright (c) 2009, Rockwell Automation, Inc.
 * All rights reserved. 
 *
 ******************************************************************************/
#include "opener_api.h"
#include "cipcommon.h"
#include "cipmessagerouter.h"
#include "endianconv.h"
#include "ciperror.h"
#include "trace.h"

CipMessageRouterRequest g_message_router_request;
CipMessageRouterResponse g_message_router_response;

/** @brief A class registry list node
 *
 * A linked list of this  object is the registry of classes known to the message router
 * for small devices with very limited memory it could make sense to change this list into an
 * array with a given max size for removing the need for having to dynamically allocate 
 * memory. The size of the array could be a parameter in the platform config file.
 */
typedef struct cip_message_router_object {
  struct cip_message_router_object *next; /*< link */
  CipClass *cip_class; /*< object */
} CipMessageRouterObject;

/** @brief Pointer to first registered object in MessageRouter*/
CipMessageRouterObject *g_first_object = 0;

/** @brief Register an Class to the message router
 *  @param cip_class Pointer to a class object to be registered.
 *  @return status      0 .. success
 *                     -1 .. error no memory available to register more objects
 */
EipStatus RegisterClass(CipClass *cip_class);

/** @brief Create Message Router Request structure out of the received data.
 * 
 * Parses the UCMM header consisting of: service, IOI size, IOI, data into a request structure
 * @param data pointer to the message data received
 * @param data_length number of bytes in the message
 * @param message_router_request pointer to structure of MRRequest data item.
 * @return status  0 .. success
 *                 -1 .. error
 */
CipError CreateMessageRouterRequestStructure(
    EipUint8 *data, EipInt16 data_length,
    CipMessageRouterRequest *message_router_request);

EipStatus CipMessageRouterInit() {
  CipClass *message_router;

  message_router = CreateCipClass(kCipMessageRouterClassCode, /* class ID*/
                                  0, /* # of class attributes */
                                  0xffffffff, /* class getAttributeAll mask*/
                                  0, /* # of class services*/
                                  0, /* # of instance attributes*/
                                  0xffffffff, /* instance getAttributeAll mask*/
                                  0, /* # of instance services*/
                                  1, /* # of instances*/
                                  "message router", /* class name*/
                                  1); /* revision */
  if (message_router == 0)
    return kEipStatusError;

  /* reserved for future use -> set to zero */
  g_message_router_response.reserved = 0;
  g_message_router_response.data = g_message_data_reply_buffer; /* set reply buffer, using a fixed buffer (about 100 bytes) */

  return kEipStatusOk;
}

/** @brief Get the registered MessageRouter object corresponding to ClassID.
 *  given a class ID, return a pointer to the registration node for that object
 *
 *  @param class_id Class code to be searched for.
 *  @return Pointer to registered message router object
 *      0 .. Class not registered
 */
CipMessageRouterObject *GetRegisteredObject(EipUint32 class_id) {
  CipMessageRouterObject *object = g_first_object; /* get pointer to head of class registration list */

  while (NULL != object) /* for each entry in list*/
  {
    OPENER_ASSERT(object->cip_class != NULL);
    if (object->cip_class->class_id == class_id)
      return object; /* return registration node if it matches class ID*/
    object = object->next;
  }
  return 0;
}

CipClass *GetCipClass(EipUint32 class_id) {
  CipMessageRouterObject *p = GetRegisteredObject(class_id);

  if (p)
    return p->cip_class;
  else
    return NULL;
}

CipInstance *GetCipInstance(CipClass *cip_class, EipUint32 instance_number) {
  CipInstance *instance; /* pointer to linked list of instances from the class object*/

  if (instance_number == 0)
    return (CipInstance *) cip_class; /* if the instance number is zero, return the class object itself*/

  for (instance = cip_class->instances; instance; instance = instance->next) /* follow the list*/
  {
    if (instance->instance_number == instance_number)
      return instance; /* if the number matches, return the instance*/
  }

  return NULL;
}

EipStatus RegisterClass(CipClass *cip_class) {
  CipMessageRouterObject **message_router_object = &g_first_object;

  while (*message_router_object)
    message_router_object = &(*message_router_object)->next; /* follow the list until p points to an empty link (list end)*/

  *message_router_object = (CipMessageRouterObject *) CipCalloc(
      1, sizeof(CipMessageRouterObject)); /* create a new node at the end of the list*/
  if (*message_router_object == 0)
    return kEipStatusError; /* check for memory error*/

  (*message_router_object)->cip_class = cip_class; /* fill in the new node*/
  (*message_router_object)->next = NULL;

  return kEipStatusOk;
}

EipStatus NotifyMR(EipUint8 *data, int data_length) {
  EipStatus eip_status = kEipStatusOkSend;
  EipByte nStatus;

  g_message_router_response.data = g_message_data_reply_buffer; /* set reply buffer, using a fixed buffer (about 100 bytes) */

  OPENER_TRACE_INFO("notifyMR: routing unconnected message\n");
  if (kCipErrorSuccess
      != (nStatus = CreateMessageRouterRequestStructure(
          data, data_length, &g_message_router_request))) { /* error from create MR structure*/
    OPENER_TRACE_ERR("notifyMR: error from createMRRequeststructure\n");
    g_message_router_response.general_status = nStatus;
    g_message_router_response.size_of_additional_status = 0;
    g_message_router_response.reserved = 0;
    g_message_router_response.data_length = 0;
    g_message_router_response.reply_service = (0x80
        | g_message_router_request.service);
  } else {
    /* forward request to appropriate Object if it is registered*/
    CipMessageRouterObject *pt2regObject;

    pt2regObject = GetRegisteredObject(
        g_message_router_request.request_path.class_id);
    if (pt2regObject == 0) {
      OPENER_TRACE_ERR(
          "notifyMR: sending CIP_ERROR_OBJECT_DOES_NOT_EXIST reply, class id 0x%x is not registered\n",
          (unsigned ) g_message_router_request.request_path.class_id);
      g_message_router_response.general_status =
          kCipErrorPathDestinationUnknown; /*according to the test tool this should be the correct error flag instead of CIP_ERROR_OBJECT_DOES_NOT_EXIST;*/
      g_message_router_response.size_of_additional_status = 0;
      g_message_router_response.reserved = 0;
      g_message_router_response.data_length = 0;
      g_message_router_response.reply_service = (0x80
          | g_message_router_request.service);
    } else {
      /* call notify function from Object with ClassID (gMRRequest.RequestPath.ClassID)
       object will or will not make an reply into gMRResponse*/
      g_message_router_response.reserved = 0;
      OPENER_ASSERT(NULL != pt2regObject->cip_class);
      OPENER_TRACE_INFO("notifyMR: calling notify function of class '%s'\n",
                        pt2regObject->cip_class->class_name);
      eip_status = NotifyClass(pt2regObject->cip_class,
                               &g_message_router_request,
                               &g_message_router_response);

#ifdef OPENER_TRACE_ENABLED
      if (eip_status == kEipStatusError) {
        OPENER_TRACE_ERR(
            "notifyMR: notify function of class '%s' returned an error\n",
            pt2regObject->cip_class->class_name);
      } else if (eip_status == kEipStatusOk) {
        OPENER_TRACE_INFO(
            "notifyMR: notify function of class '%s' returned no reply\n",
            pt2regObject->cip_class->class_name);
      } else {
        OPENER_TRACE_INFO(
            "notifyMR: notify function of class '%s' returned a reply\n",
            pt2regObject->cip_class->class_name);
      }
#endif
    }
  }
  return eip_status;
}

CipError CreateMessageRouterRequestStructure(
    EipUint8 *data, EipInt16 data_length,
    CipMessageRouterRequest *message_router_request) {
  int number_of_decoded_bytes;

  message_router_request->service = *data;
  data++;  /*TODO: Fix for 16 bit path lengths (+1 */
  data_length--;

  number_of_decoded_bytes = DecodePaddedEPath(
      &(message_router_request->request_path), &data);
  if (number_of_decoded_bytes < 0) {
    return kCipErrorPathSegmentError;
  }

  message_router_request->data = data;
  message_router_request->data_length = data_length - number_of_decoded_bytes;

  if (message_router_request->data_length < 0)
    return kCipErrorPathSizeInvalid;
  else
    return kCipErrorSuccess;
}

void DeleteAllClasses(void) {
  CipMessageRouterObject *message_router_object = g_first_object; /* get pointer to head of class registration list */
  CipMessageRouterObject *message_router_object_to_delete;
  CipInstance *instance, *instance_to_delete;

  while (NULL != message_router_object) {
    message_router_object_to_delete = message_router_object;
    message_router_object = message_router_object->next;

    instance = message_router_object_to_delete->cip_class->instances;
    while (NULL != instance) {
      instance_to_delete = instance;
      instance = instance->next;
      if (message_router_object_to_delete->cip_class->number_of_attributes) /* if the class has instance attributes */
      { /* then free storage for the attribute array */
        CipFree(instance_to_delete->attributes);
      }
      CipFree(instance_to_delete);
    }

    /*clear meta class data*/
    CipFree(
        message_router_object_to_delete->cip_class->m_stSuper.cip_class
            ->class_name);
    CipFree(
        message_router_object_to_delete->cip_class->m_stSuper.cip_class
            ->services);
    CipFree(message_router_object_to_delete->cip_class->m_stSuper.cip_class);
    /*clear class data*/
    CipFree(message_router_object_to_delete->cip_class->m_stSuper.attributes);
    CipFree(message_router_object_to_delete->cip_class->services);
    CipFree(message_router_object_to_delete->cip_class);
    CipFree(message_router_object_to_delete);
  }
  g_first_object = NULL;
}