CREATE MIGRATION m1vegpxb3odf7j6rsioor2j5zcassvioypuixdcfujquycuufa3k2a
    ONTO initial
{
  CREATE TYPE default::Person {
      CREATE REQUIRED PROPERTY name: std::str;
  };
  CREATE TYPE default::Movie {
      CREATE MULTI LINK actors: default::Person;
      CREATE REQUIRED PROPERTY title: std::str;
  };
};
