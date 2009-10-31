#ifndef __DETECT_ADDRESS_H__
#define __DETECT_ADDRESS_H__

/* prototypes */
void DetectAddressRegister (void);
void DetectAddressPrintMemory(void);

DetectAddresssHead *DetectAddresssHeadInit(void);
void DetectAddresssHeadFree(DetectAddresssHead *);
void DetectAddresssHeadCleanup(DetectAddresssHead *);

int DetectAddressParse(DetectAddresssHead *, char *);

DetectAddress *DetectAddressInit(void);
void DetectAddressFree(DetectAddress *);

void DetectAddressCleanupList (DetectAddress *);
int DetectAddressAdd(DetectAddress **, DetectAddress *);
void DetectAddressPrintList(DetectAddress *);

int DetectAddressInsert(DetectEngineCtx *, DetectAddresssHead *, DetectAddress *);
int DetectAddressJoin(DetectEngineCtx *, DetectAddress *, DetectAddress *);

DetectAddress *DetectAddressLookupInHead(DetectAddresssHead *, Address *);
DetectAddress *DetectAddressLookupInList(DetectAddress *, DetectAddress *);

/** \brief address only copy of ag */
DetectAddress *DetectAddressCopy(DetectAddress *);
/** \brief debugging: print a detect address */
void DetectAddressPrint(DetectAddress *);
/** \brief compare the address part of two DetectAddress objects */
int DetectAddressCmp(DetectAddress *, DetectAddress *);

#endif /* __DETECT_ADDRESS_H__ */

