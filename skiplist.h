#include<iostream>
#include<cmath>
#include<cstring>
#include<mutex>
#include<fstream>
#include <filesystem>

#define STORE_FILE "store/dumpFile"

std::mutex mtx;  //代表互斥锁 ，保持线程同步
std::string delimiter=":";  //存放到STORE_FILE中时，将delimiter也存入进文件中，用于get_key_value_from_string的key与value区分

template<typename K,typename V>
class Node{
public:
    Node(){}
    Node(K k,V v,int);
    ~Node();
    K get_key() const;
    V get_value() const;
    void set_value(V);

    Node <K,V> **forward;  //forward是指针数组，用于指向下一层 例如  forward[0]是指向第一层，forward[1]指向上一层
    int node_level;
private:
     K key;
     V value;
};
template<typename K,typename V>
Node<K,V>::Node(const K k, const V v, int level)
{
    this->key=k;
    this->value=v;
    this->node_level=level;
    this->forward=new Node<K,V> *[level+1];
    memset(this->forward,0,sizeof(Node<K,V>*)*(level+1));
};
template<typename  K,typename V>
Node<K,V>::~Node()
{
    delete []forward;
};
template<typename K,typename V>
K Node<K,V>::get_key() const {
    return key;
};
template<typename K,typename V>
V Node<K,V>::get_value() const {
    return value;
};
template<typename K,typename V>
void Node<K,V>::set_value(V value)
{
    this->value=value;
};

template<typename K,typename V>
class SkipList{
public:
    SkipList(int);
    ~SkipList();
    int get_random_level();
    Node<K,V>*create_node(K,V,int);
    int insert_element(K,V);
    void display_list();
    bool search_element(K);
    void delete_element(K);
    void dump_file();
    void load_file();
    int size();
private:
    void get_key_value_from_string(const std::string &str,std::string*key,std::string *value);
    bool is_valid_string(const std::string &str);
private:
    int _max_level;              //跳表的最大层级
    int _skip_list_level;        //当前跳表的有效层级
    Node<K,V> *_header;          //表示跳表的头节点
    std::ofstream _file_writer;  //默认以输入(writer)方式打开文件。
    std::ifstream _file_reader;  //默认以输出(reader)方式打开文件。
    int _element_count;          //表示跳表中元素的数量
};

//create_node函数：根据给定的键、值和层级创建一个新节点，并返回该节点的指针
template<typename K,typename V>
Node<K,V> *SkipList<K,V>::create_node(const K k, const V v, int level)
{
    Node<K,V>*n=new Node<K,V>(k,v,level);
    return n;
}

//insert_element 函数：插入一个新的键值对到跳表中。通过遍历跳表，找到插入位置，并根据随机层级创建节点。
//如果键已存在，则返回 1，表示插入失败；否则，插入成功，返回 0。
template<typename K,typename V>
// 插入一个元素到跳表中
int SkipList<K,V>::insert_element(const K key, const V value) {
    // 加锁，保证线程安全
    mtx.lock();
    // 当前节点指针，初始化为跳表的头节点
    Node<K,V> *current = this->_header;
    // 更新节点指针数组，用于存储每一层需要插入点节点的位置
    Node<K,V> *update[_max_level];
    // 初始化更新节点指针数组为0
    memset(update, 0, sizeof(Node<K,V>*) * (_max_level + 1));

    // 从最高层开始遍历，找到每一层需要插入的位置
    for(int i = _skip_list_level; i >= 0; i--) {
        // 找到当前层小于key的最大节点
        while(current->forward[i] != NULL && current->forward[i]->get_key() < key) {
            current = current->forward[i];
        }
        // 记录当前层需要更新的节点
        update[i] = current;
    }

    // 找到第0层的目标节点
    current = current->forward[0];
    // 如果目标节点存在且key值相等，说明元素已存在，插入失败
    if(current != NULL && current->get_key() == key) {
        std::cout << "key:" << key << ",exists" << std::endl;
        // 解锁
        mtx.unlock();
        return 1;
    }

    // 添加的值没有在跳表中，进行插入操作
    if(current == NULL || current->get_key() != key) {
        // 生成一个随机层级
        int random_level = get_random_level();
        // 如果随机层级大于当前跳表的有效层级
        if(random_level > _skip_list_level) {
            // 从当前有效层级+1开始，将更新节点指针数组中的节点设置为头节点
            for(int i = _skip_list_level + 1; i < random_level + 1; i++) {
                update[i] = _header;
            }
            // 更新跳表的有效层级
            _skip_list_level = random_level;
        }
        // 创建一个新节点
        Node<K,V>* inserted_node = create_node(key, value, random_level);
        // 从第0层开始，更新节点的forward指针，插入新节点
        for(int i = 0; i <= random_level; i++) {
            inserted_node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = inserted_node;
        }
        // 输出插入成功信息
        std::cout << "Successfully inserted key:" << key << ",value:" << value << std::endl;
        // 更新元素数量
        _element_count++;
    }
    // 解锁
    mtx.unlock();
    return 0;
}


//display_list函数：输出跳表包含的内容、循环_skip_list_level(有效层级)、从_header头节点开始、结束后指向下一节点
template<typename K,typename V>
void SkipList<K,V>::display_list()
{
    std::cout<<"\n*****SkipList*****"<<"\n";
    for(int i=0;i<_skip_list_level;i++)
    {
        Node<K,V>*node=this->_header->forward[i];
        std::cout<<"Level"<<i<<":";
        while(node!=NULL)
        {
            std::cout<<node->get_key()<<":"<<node->get_value()<<";";
            node=node->forward[i];
        }
        std::cout<<std::endl;
    }
}

//dump_file 函数：将跳跃表的内容持久化到文件中。遍历跳跃表的每个节点，将键值对写入文件。
//其主要作用就是将跳表中的信息存储到STORE_FILE文件中，node指向forward[0]，每一次结束后再将node指向node.forward[0]。
template<typename K,typename V>
// 将跳跃表的内容持久化到文件中。遍历跳跃表的每个节点，将键值对写入文件。
void SkipList<K,V>::dump_file()
{
    // 输出提示信息，表示开始将跳表内容写入文件
    std::cout << "dump_file-----------" << std::endl;
    // 打开文件，准备写入数据
    _file_writer.open(STORE_FILE);
    // 从跳表的第一层开始遍历
    Node<K,V>* node = this->_header->forward[0];
    // 遍历跳表的每一层，直到节点为空
    while (node != NULL)
    {
        // 将当前节点的键和值写入文件，格式为 "key:value"
        _file_writer << node->get_key() << ":" << node->get_value() << "\n";
        // 输出当前节点的键和值，格式为 "key:value"
        std::cout << node->get_key() << ":" << node->get_value() << "\n";
        // 移动到下一个节点
        node = node->forward[0];
    }
    // 刷新文件缓冲区，确保数据写入文件
    _file_writer.flush();
    // 关闭文件
    _file_writer.close();
    return;
}


//将文件中的内容转到跳表中、每一行对应的是一组数据，数据中有：分隔，还需要get_key_value_from_string(line,key,value)将key和value分开。
//直到key和value为空时结束，每组数据分开key、value后通过insert_element()存到跳表中来
template<typename K,typename V>
// 从文件中加载数据到跳表中
void SkipList<K,V>::load_file()
{
    // 打开文件，准备读取数据
    _file_reader.open(STORE_FILE);
    // 输出提示信息，表示开始从文件中加载数据
    std::cout << "load_file----------" << std::endl;
    // 用于存储读取的每一行数据
    std::string line;
    // 用于存储解析出的键
    std::string *key = new std::string();
    // 用于存储解析出的值
    std::string *value = new std::string();
    // 逐行读取文件内容
    while (getline(_file_reader, line))
    {
        // 从当前行中解析出键和值
        get_key_value_from_string(line, key, value);
        // 如果键或值为空，则跳过当前行
        if (key->empty() || value->empty())
        {
            continue;
        }
        // 将字符串类型的键转换为整数类型
        int target = 0;
        std::string str_key = *key;
        for (int i = 0; i < str_key.size(); i++)
        {
            target = target * 10 + str_key[i] - '0';
        }
        // 将解析出的键值对插入到跳表中
        int Yes_No = insert_element(target, *value);
        // 输出插入的键值对信息
        std::cout << "key:" << *key << "value:" << *value << std::endl;
    }
    // 关闭文件
    _file_reader.close();
}


//表示跳表中元素的数量
template<typename K,typename V>
int SkipList<K,V>::size() {
    return _element_count;
}

//从STORE_FILE文件读取时，每一行将key和value用 ：分开，此函数将每行的key和value分割存入跳表中
template<typename K,typename V>
void SkipList<K,V>::get_key_value_from_string(const std::string &str, std::string *key, std::string *value)
{
    if(!is_valid_string(str)) return ;
    *key=str.substr(0,str.find(delimiter));
    *value=str.substr(str.find(delimiter)+1,str.length());
}

//判断从get_key_value_from_string函数中分割的字符串是否正确
template<typename K,typename V>
bool SkipList<K,V>::is_valid_string(const std::string &str)
{
    if(str.empty())
    {
        return false;
    }
    if(str.find(delimiter)==std::string::npos)
    {
        return false;
    }
    return true;
}

//遍历跳表找到每一层需要删除的节点，将前驱指针往前更新，遍历每一层时，都需要找到对应的位置
//前驱指针更新完，还需要将全为0的层删除
template<typename K,typename V>
// 删除跳表中的元素
void SkipList<K,V>::delete_element(K key)
{
    mtx.lock();
    // 当前节点指针
    Node<K,V>*current=this->_header;
    // 更新节点指针数组
    Node<K,V>*update[_max_level+1];
    // 初始化更新节点指针数组
    memset(update,0,sizeof(Node<K,V>*)*(_max_level+1));
    // 从最高层开始遍历
    for(int i=_skip_list_level;i>=0;i--)
    {
        // 找到当前层小于key的最大节点
        while(current->forward[i]!=NULL&&current->forward[i]->get_key()<key)
        {
            current=current->forward[i];
        }
        // 记录当前层需要更新的节点
        update[i]=current;
    }
    // 找到第0层的目标节点
    current=current->forward[0];
    // 如果目标节点存在且key值相等
    if(current!=NULL&&current->get_key()==key)
    {
        // 从第0层开始更新节点的forward指针
        for(int i=0;i<=_skip_list_level;i++) {
            if (update[i]->forward[i] != current) {
                break;
            }
            update[i]->forward[i] = current->forward[i];
        }
        // 更新跳表的层数
        while(_skip_list_level>0&&_header->forward[_skip_list_level]==0)
        {
            _skip_list_level--;
        }
        // 输出删除成功信息
        std::cout<<"Successfully deleted key"<<key<<std::endl;
        // 更新元素数量
        _element_count--;
    }
    mtx.unlock();
    return ;
}


//遍历每一层，从顶层开始，找到每层对应的位置，然后进入下一层开始查找，直到查找到对应的key
//如果找到return true 输出Found  否则 return false ，输出Not Found
template<typename K,typename V>
bool SkipList<K,V>::search_element(K key)
{
    std::cout<<"search_element------------"<<std::endl;
    Node<K,V> *current=_header;
    for(int i=_skip_list_level;i>=0;i--)
    {
        while(current->forward[i]&&current->forward[i]->get_key()<key)
        {
            current=current->forward[i];
        }
    }
    current=current->forward[0];
    if(current and current->get_key()==key)
    {
        std::cout<<"Found key:"<<key<<",value:"<<current->get_value()<<std::endl;
        return true;
    }
    std::cout<<"Not Found Key:"<<key<<std::endl;
    return false;
}
template<typename K,typename V>

SkipList<K,V>::SkipList(int max_level)
{
    this->_max_level=max_level;
    this->_skip_list_level=0;
    this->_element_count=0;
    K k;
    V v;
    this->_header=new Node<K,V>(k,v,_max_level);
};
//释放内存，关闭_file_writer  _file_reader
template<typename K,typename V>
SkipList<K,V>::~SkipList()
{
    if(_file_writer.is_open())
    {
        _file_writer.close();
    }
    if(_file_reader.is_open())
    {
        _file_reader.close();
    }
    delete _header;
}
//生成一个随机层级。从第一层开始，每一层以 50% 的概率加入
template<typename K,typename V>
int SkipList<K,V>::get_random_level()
{
    int k=1;
    while(rand()%2)
    {
        k++;
    }
    k=(k<_max_level)?k:_max_level;
    return k;
};

