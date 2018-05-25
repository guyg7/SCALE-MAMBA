/*
Copyright (c) 2017, The University of Bristol, Senate House, Tyndall Avenue, Bristol, BS8 1TH, United Kingdom.
Copyright (c) 2018, COSIC-KU Leuven, Kasteelpark Arenberg 10, bus 2452, B-3001 Leuven-Heverlee, Belgium.

All rights reserved
*/

#include <errno.h>
#if !defined(__MACH__)
#include <malloc.h>
#else
#include <stdlib.h>
#endif
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <sys/socket.h>
#include <sys/types.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <resolv.h>

#include <arpa/inet.h>

#include "openssl/err.h"
#include "openssl/ssl.h"
#include <fstream>
#include <mutex>
using namespace std;

#include "Exceptions/Exceptions.h"
#include "Networking.h"
#include "Player.h"

SSL_CTX *InitCTX(void)
{
  const SSL_METHOD *method;
  SSL_CTX *ctx;

  method= TLS_method();     /* create new server-method instance */
  ctx= SSL_CTX_new(method); /* create new context from method */

  if (ctx == NULL)
    {
      ERR_print_errors_fp(stdout);
      throw SSL_error("InitCTX");
    }

  SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);

  return ctx;
}

void LoadCertificates(SSL_CTX *ctx, const char *CertFile, const char *KeyFile)
{
  /* set the local certificate from CertFile */
  if (SSL_CTX_use_certificate_file(ctx, CertFile, SSL_FILETYPE_PEM) <= 0)
    {
      ERR_print_errors_fp(stdout);
      throw SSL_error("LoadCertificates 1");
    }
  /* set the private key from KeyFile (may be the same as CertFile) */
  if (SSL_CTX_use_PrivateKey_file(ctx, KeyFile, SSL_FILETYPE_PEM) <= 0)
    {
      ERR_print_errors_fp(stdout);
      throw SSL_error("LoadCertificates 2");
    }
  /* verify private key */
  if (!SSL_CTX_check_private_key(ctx))
    {
      throw SSL_error("Private key does not match the public certificate");
    }
}

void ShowCerts(SSL *ssl, const string CommonName, bool verbose= false)
{
  X509 *cert;
  char *line;

  cert= SSL_get_peer_certificate(ssl); /* Get certificates (if available) */
  if (cert != NULL)
    {
      if (verbose)
        {
          printf("Server certificates:\n");
          line= X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
          printf("Subject: %s\n", line);
          free(line);
          line= X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
          printf("Issuer: %s\n", line);
          free(line);
        }

      char buffer[256];
      X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName,
                                buffer, 256);
      string name(buffer);
      if (verbose)
        {
          printf("Subject Comman Name:  %s\n", buffer);
        }
      if (name.compare(CommonName) != 0)
        {
          throw SSL_error("Common name does not match what I was expecting");
        }

      X509_free(cert);
    }
  else
    printf("No certificates.\n");
}

void Init_SSL_CTX(SSL_CTX *&ctx, int me, const SystemData &SD)
{
  // Initialize the SSL library
  OPENSSL_init_ssl(
      OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
  ctx= InitCTX();

  // Load in my certificates
  string str_crt= "Cert-Store/" + SD.PlayerCRT[me];
  string str_key= str_crt.substr(0, str_crt.length() - 3) + "key";
  LoadCertificates(ctx, str_crt.c_str(), str_key.c_str());

  // Turn on client auth via cert
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     NULL);

  // Load in root CA
  string str= "Cert-Store/" + SD.RootCRT;
  SSL_CTX_set_client_CA_list(ctx, SSL_load_client_CA_file(str.c_str()));
  SSL_CTX_load_verify_locations(ctx, str.c_str(), NULL);
}

Player::Player(int mynumber, const SystemData &SD, int thread, SSL_CTX *ctx,
               vector<int> &csockets, bool verbose)
{
  G.ReSeed(thread);

  me= mynumber;
  ssl.resize(SD.n);

  // When communicating with player i, player me acts as server when i<me
  for (unsigned int i= 0; i < SD.n; i++)
    {
      if (i != me)
        {
          ssl[i]= SSL_new(ctx); /* get new SSL state with context */
          if (i < me)
            { /* set connection socket to SSL state */
              int ret= SSL_set_fd(ssl[i], csockets[i]);
              if (ret == 0)
                {
                  printf(
                      "S: Player %d failed to SSL_set_fd with player %d in thread %d\n",
                      mynumber, i, thread);
                  throw SSL_error("SSL_set_fd");
                }
              if (verbose)
                {
                  printf("S: Player %d going SSL with player %d at %s in thread %d\n",
                         mynumber, i, SD.IP[i].c_str(), thread);
                }
              /* do SSL-protocol accept */
              ret= SSL_accept(ssl[i]);
              if (ret <= 0)
                {
                  printf("S: Error in player %d accepting to player %d at address %s "
                         "in thread %d\n",
                         mynumber, i, SD.IP[i].c_str(), thread);
                  ERR_print_errors_fp(stdout);
                  throw SSL_error("SSL_accept");
                }
              if (verbose)
                {
                  printf("S: Player %d connected to player %d in thread %d with %s "
                         "encryption\n",
                         mynumber, i, thread, SSL_get_cipher(ssl[i]));
                }
            }
          else
            { // Now client side stuff
              int ret= SSL_set_fd(ssl[i], csockets[i]);
              if (ret == 0)
                {
                  printf(
                      "C: Player %d failed to SSL_set_fd with player %d in thread %d\n",
                      mynumber, i, thread);
                  throw SSL_error("SSL_set_fd");
                }
              if (verbose)
                {
                  printf("C: Player %d going SSL with player %d at %s in thread %d\n",
                         mynumber, i, SD.IP[i].c_str(), thread);
                }
              /* do SSL-protocol connect */
              ret= SSL_connect(ssl[i]);
              if (ret <= 0)
                {
                  printf("C: Error player %d connecting to player %d at address %s in "
                         "thread %d\n",
                         mynumber, i, SD.IP[i].c_str(), thread);
                  ERR_print_errors_fp(stdout);
                  throw SSL_error("SSL_connect");
                }
              if (verbose)
                {
                  printf("C: Player %d connected to player %d in thread %d with %s "
                         "encryption\n",
                         mynumber, i, thread, SSL_get_cipher(ssl[i]));
                }
            }
          ShowCerts(ssl[i], SD.PlayerCN[i]); /* get cert and test common name */
        }
    }
}

Player::~Player()
{
  for (unsigned int i= 0; i < ssl.size(); i++)
    {
      if (i != me)
        {
          SSL_free(ssl[i]);
        }
    }
}

void Player::send_all(const string &o, bool verbose) const
{
  uint8_t buff[4];
  encode_length(buff, o.length());
  for (unsigned int i= 0; i < ssl.size(); i++)
    {
      if (i != me)
        {
          if (SSL_write(ssl[i], buff, 4) != 4)
            {
              throw sending_error();
            }
          if (SSL_write(ssl[i], o.c_str(), o.length()) != (int) o.length())
            {
              throw sending_error();
            }
          if (verbose)
            {
              printf("Sent to player %d : ", i);
              for (unsigned int j= 0; j < 4; j++)
                {
                  printf("%.2X", (uint8_t) buff[j]);
                }
              printf(" : ");
              for (unsigned int j= 0; j < o.size(); j++)
                {
                  printf("%.2X", (uint8_t) o.c_str()[j]);
                }
              printf("\n");
            }
        }
    }
}

void Player::send_to_player(int player, const string &o) const
{
  uint8_t buff[4];
  encode_length(buff, o.length());
  if (SSL_write(ssl[player], buff, 4) != 4)
    {
      throw sending_error();
    }
  if (SSL_write(ssl[player], o.c_str(), o.length()) != (int) o.length())
    {
      throw sending_error();
    }
}

void receive(SSL *ssl, uint8_t *data, int len)
{
  int i= 0, j;
  while (len - i > 0)
    {
      j= SSL_read(ssl, data + i, len - i);
      if (j <= 0)
        {
          /*
             int e0=SSL_get_error(ssl,j);
             int e1=ERR_get_error();
             printf("SSL_READ error : %d : %d % d : Was trying to read % d bytes out
             of % d bytes \n",j,e0,e1,len-i,len);
             perror("Some Error" );
          */
          throw receiving_error();
        }
      i= i + j;
    }
  if (len - i != 0)
    {
      throw receiving_error();
    }
}

void Player::receive_from_player(int i, string &o, bool verbose) const
{
  uint8_t buff[4];
  receive(ssl[i], buff, 4);
  int nlen= decode_length(buff);
  uint8_t *sbuff= new uint8_t[nlen];
  receive(ssl[i], sbuff, nlen);
  o.assign((char *) sbuff, nlen);
  if (verbose)
    {
      printf("Received from player %d : ", i);
      for (unsigned int j= 0; j < 4; j++)
        {
          printf("%.2X", (uint8_t) buff[j]);
        }
      printf(" : ");
      for (unsigned int j= 0; j < o.size(); j++)
        {
          printf("%.2X", (uint8_t) o.c_str()[j]);
        }
      printf("\n");
      for (int j= 0; j < nlen; j++)
        {
          printf("%.2X", (uint8_t) sbuff[j]);
        }
      printf("\n");
    }
  delete[] sbuff;
}

void Player::load_mac_keys(int num)
{
  mac_keys.resize(num);
  stringstream ss;
  ss << "Data/MKey-" << me << ".key";
  ifstream inp(ss.str().c_str());
  if (inp.fail())
    {
      throw file_error(ss.str());
    }
  for (int i= 0; i < num; i++)
    {
      inp >> mac_keys[i];
    }
  inp.close();
}

/* This is deliberately weird to avoid problems with OS max buffer
 * size getting in the way
 */
void Player::Broadcast_Receive(vector<string> &o) const
{
  for (unsigned int i= 0; i < ssl.size(); i++)
    {
      if (i > me)
        {
          send_to_player(i, o[me]);
        }
      else if (i < me)
        {
          receive_from_player(i, o[i]);
        }
    }
  for (unsigned int i= 0; i < ssl.size(); i++)
    {
      if (i < me)
        {
          send_to_player(i, o[me]);
        }
      else if (i > me)
        {
          receive_from_player(i, o[i]);
        }
    }
}

void Player::Send_Distinct_And_Receive(vector<string> &o) const
{
  for (unsigned int i= 0; i < ssl.size(); i++)
    {
      if (i != me)
        {
          send_to_player(i, o[i]);
        }
    }
  for (unsigned int i= 0; i < ssl.size(); i++)
    {
      if (i != me)
        {
          receive_from_player(i, o[i]);
        }
    }
}
