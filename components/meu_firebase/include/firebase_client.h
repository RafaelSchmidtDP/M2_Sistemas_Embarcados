#ifndef FIREBASE_CLIENT_H
#define FIREBASE_CLIENT_H

#define FIREBASE_URL  "https://contadorparque-default-rtdb.firebaseio.com/contagem.json"

bool enviar_para_firebase(int entradas, int saidas);

#endif