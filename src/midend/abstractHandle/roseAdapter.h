#ifndef rose_adapter_INCLUDED
#define rose_adapter_INCLUDED


#include <iostream>
#include <string>

#include "abstract_handle.h"

class roseNode: public AbstractHandle::abstract_node 
{
 public:
   roseNode(SgNode* snode):mNode(snode){};
   virtual ~roseNode(){};
   SgNode* getNode() const {return mNode; }
   virtual std::string getConstructTypeName() const;
   virtual bool hasSourcePos() const;
   virtual bool hasName() const;
   virtual std::string getName() const;
   virtual AbstractHandle::abstract_node* getFileNode() const;
   virtual AbstractHandle::abstract_node* getParent() const;
   virtual AbstractHandle::abstract_node* findNode(std::string construct_type_str, AbstractHandle::specifier mspecifier) const;
   virtual std::string getFileName() const;
   virtual AbstractHandle::source_position getStartPos() const;
   virtual AbstractHandle::source_position getEndPos() const;

  virtual size_t getNumbering (const AbstractHandle::abstract_node* another_node) const;
  virtual std::string toString() const;
  virtual bool operator == (const abstract_node & x) const;
protected:
    SgNode* mNode;
};

#endif
